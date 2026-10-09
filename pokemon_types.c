#include <furi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <stream/stream.h>
#include <stream/file_stream.h>

// Title shown at the top of the main menu (the screen after a generation is confirmed).
#define MAIN_MENU_TITLE "Pokemon Type Checker"

// How many types the app knows about in total (Normal ... Fairy). Each
// generation uses the first 'type_count' of them; see generations[] below.
#define TYPE_COUNT 18

#define POKEDEX_DATA_DIR EXT_PATH("apps_data/pokemon_types")

// Every entry of a list becomes a menu item held in RAM, so a list shows at most
// this many entries at a time; longer lists get "Previous page" / "Next page" items.
#define PT_PAGE 160
// Ids of those two items (real entries use their position in the list, below 2000).
#define PT_ID_PREV_PAGE 100000
#define PT_ID_NEXT_PAGE 100001

typedef enum {
    PtViewSubmenu,
    PtViewResult,
    PtViewTextInput,
    PtViewDetail,
    PtViewTitle,
} PtView;

typedef enum {
    StepTitle,
    StepGenSelect,
    StepGenConfirm,
    StepMainMenu,
    StepTypeAttack,
    StepDefenseOptions,
    StepDefenderSource, // Search or Browse, when choosing a defender
    StepFormPick,
    StepBrowseBy, // Pokedex Browse: by number, type or ability
    StepBrowseType,
    StepBrowseLetter,
    StepBrowseAbility,
    StepMatchups,
    StepTypeDefense1,
    StepTypeDefense2,
    StepTypeResult,
    StepPokedexSearch,
    StepPokedexGroups,
    StepPokedexResults,
    StepPokedexDetail,
    StepPokedexError,
    StepGenMem, // memory figures, opened from the Choose Generation confirm screen
    StepMemError, // "not enough memory" for the generation just confirmed
    StepMainMem, // memory figures for the loaded generation, from the main menu
} PtStep;

// Sentinel for "no second type" in PokemonEntry.type2.
#define PT_NO_TYPE 0xFF

typedef struct {
    uint16_t number;
    char name[16];
    // Types as indexes into type_names[] (1 byte each instead of a 12-character
    // text each). type2 is PT_NO_TYPE for single-type Pokemon.
    uint8_t type1;
    uint8_t type2;
    // Offset into app->evo_pool where this entry's evolution text starts. The
    // pool holds "EvolvesTo\0EvoMethod\0" back to back, so the (few) entries
    // that evolve don't force every entry to reserve space for long strings.
    // Offset 0 is a shared "-\0-\0" meaning "does not evolve".
    uint16_t evo_offset;
    // Offset into the same pool of this entry's "FormData" text: one token per form
    // (see pt_parse_form), rows separated by '|'. 0 = the Pokemon has no alternate forms.
    uint16_t formdata_offset;
    // Abilities, as indexes into abilities[] below (0 = none). [0] and [1] are
    // the normal abilities, [2] is the Hidden Ability. Generations 1-2 have none.
    uint16_t ability[3]; // 16 bits: Generations 8-9 bring the ability table past 255 entries
} PokemonEntry;

// An ability. 'effect' is only filled in for abilities that change how a type
// matchup plays out in battle (an immunity, a resistance, a type change); every
// other ability is shown by name alone. 'effect_from_gen' says from which
// generation the effect applies (0 = always): for example Lightning Rod only
// absorbs Electric attacks from Generation 5 on, so earlier generations show
// just its name.
typedef struct {
    const char* name;
    const char* effect;
    uint8_t effect_from_gen;
} PtAbility;

// One entry on the "Pokedex Browse" group list: a range of Pokedex numbers.
typedef struct {
    const char* label;
    int first;
    int last;
} PtGroup;

// One Pokemon form with everything resolved: the form chosen as the defender in the
// type checker, and the form shown on the detail screen.
typedef struct {
    char name[16];
    char label[24]; // form name, "" for a Pokemon without forms
    int t1;
    int t2; // -1 = single type
    int ab_count;
    char ab[3][20];
    bool ab_hidden[3];
    bool ab_unknown; // the form's abilities are not known (the data says "?")
    bool inherit_types; // (parse only) the token gave no types: use the Pokemon's own
    bool inherit_ab; // (parse only) the token gave no abilities: use the Pokemon's own
    char note[64]; // how the form is obtained / what it evolves into, "" if none
} PtDefender;

// What the custom detail screen needs in order to draw itself. A View's draw
// callback is handed only this "model" (not the app), so everything it shows
// has to be reachable from here.
typedef struct {
    const PokemonEntry* entry; // points into app->pokedex (not a copy)
    const char* evolves_to; // points into app->evo_pool: "A" or "A/B/C"
    const char* evo_method; // points into app->evo_pool: one shared method, or one per target
    const char* formdata; // points into app->evo_pool: the form tokens, or NULL when there are none
    PtDefender form; // the form being shown (valid when has_form)
    bool has_form;
    int generation; // 1-based number of the generation being browsed
    int position; // 0-based position within the list being browsed
    int total; // number of entries in that list
    int scroll; // how many text rows the body of the screen is scrolled down
    bool pick; // choosing a defender for the type checker: show the "OK: pick" hint
    bool matchup_hint; // browsing: end the body with "OK: Matchups"
} DetailModel;

// Everything that differs from one generation of the games to the next.
// To add a generation: add a type chart (if its matchups differ), a CSV data
// file, a list of Browse groups, and one entry in generations[] below. Any new
// abilities go at the end of abilities[]; any new type goes at the end of
// type_names[] (or is already there).
typedef struct {
    const char* menu_label; // line on the "Choose Generation" screen
    const char* dialog_title; // header of the confirmation dialog
    const char* games; // one game per line ("\n" separated), shown in the dialog
    int type_count; // types that exist: the first N entries of type_names
    const uint8_t (*chart)[TYPE_COUNT]; // type chart used by this generation
    int dex_count; // number of Pokemon in this generation's data file
    int evo_pool_size; // bytes needed for all evolution and forms text in that file (plus a little spare)
    const PtGroup* groups; // how "Pokedex Browse" splits the list (usually one group per region)
    int group_count;
    const char* csv_name; // data file inside apps_data/pokemon_types/ on the SD card
} GenerationInfo;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    Widget* widget;
    TextInput* text_input;
    View* detail_view; // custom screen: lets us handle the arrow keys ourselves
    View* title_view; // custom screen: the title page
    int saved_gen; // generation index remembered from last time, -1 = none

    PtStep step;

    // Generation state
    int gen_choice; // index into generations[] currently being considered
    // Heap figures (bytes) taken just before and just after the Pokedex was loaded,
    // for the "Memory Info" screen.
    size_t mem_free_before;
    size_t mem_free_after;
    const GenerationInfo* gen; // the confirmed generation (NULL until confirmed)

    // Type effectiveness state
    int attack_type;
    int defense_type1;
    int defense_type2; // -1 = no second type
    bool defender_is_pokemon; // the defender came from the Pokedex (so abilities count)
    bool pick_mode; // browsing the Pokedex to choose a defender
    bool ignore_abilities; // attacker has Mold Breaker / Teravolt / Turboblaze
    int options_choice; // cursor on the Defense Options screen
    PtDefender defender;

    // Pokedex state (allocated when a generation is confirmed, freed when you leave it)
    PokemonEntry* pokedex;
    int pokedex_capacity;
    int pokedex_count;
    char* evo_pool; // evolution text for every entry, see PokemonEntry.evo_offset
    int evo_pool_capacity;
    int evo_pool_used;
    bool pokedex_loaded;
    int* filtered_indices;
    int filtered_count;
    int selected_position; // index into filtered_indices of the entry being viewed
    bool browse_mode; // true = list came from "Pokedex Browse", false = from a search
    int group_choice; // which group of the generation Browse is showing (index into gen->groups)
    char list_title[16]; // first word of a list's header: "Results", "Fire"...
    int browse_kind; // 0 = by number, 1 = by type, 2 = by ability
    int browse_arg; // the type or ability being browsed
    char browse_letter; // first letter of the ability list being browsed
    bool matchup_view; // OK on a Pokemon's page opens its matchups (not choosing a defender)
    int detail_form; // form shown on the detail screen: index into the entry's FormData, -1 = none
    bool detail_explicit; // that form was chosen (on the form list, or as a row of a Type/Ability list)
    char search_buffer[20];
    char results_header[64];
} PokemonTypesApp;

// Ordered by when each type first appeared in the games, so "the types that
// exist in generation N" is always just the first few entries of this list.
static const char* const type_names[TYPE_COUNT] = {
    "Normal",
    "Fire",
    "Water",
    "Electric",
    "Grass",
    "Ice",
    "Fighting",
    "Poison",
    "Ground",
    "Flying",
    "Psychic",
    "Bug",
    "Rock",
    "Ghost",
    "Dragon",
    "Dark", // added in Generation 2
    "Steel", // added in Generation 2
    "Fairy"}; 
// Position of a type's name in type_names[] (case-insensitive), or PT_NO_TYPE
// if the text is not a type ("-" for "no second type" lands here too).
static uint8_t pt_type_index(const char* name) {
    for(uint8_t i = 0; i < TYPE_COUNT; i++) {
        if(strcasecmp(name, type_names[i]) == 0) return i;
    }
    return PT_NO_TYPE;
}
// added in Generation 6

// Every ability the app knows about (the Generation 9 set; later
// generations append theirs). Pokedex entries refer to these by position, and
// the CSV files name them exactly as spelled here.
//
// Only abilities that change a type matchup get an 'effect' text. To describe
// another one, replace its NULL with a short string, for example:
//   {"Scrappy", "Normal & Fighting hit Ghost"},
// or add a third number to use it only from that generation on:
//   {"Lightning Rod", "Electric immunity", 5},
static const PtAbility abilities[] = {
    {"-", NULL, 0}, // 0 = no ability
    {"Stench", NULL, 0},
    {"Drizzle", NULL, 0},
    {"Speed Boost", NULL, 0},
    {"Battle Armor", NULL, 0},
    {"Sturdy", NULL, 0},
    {"Damp", NULL, 0},
    {"Limber", NULL, 0},
    {"Sand Veil", NULL, 0},
    {"Static", NULL, 0},
    {"Volt Absorb", "Electric immunity", 0},
    {"Water Absorb", "Water immunity", 0},
    {"Oblivious", NULL, 0},
    {"Cloud Nine", NULL, 0},
    {"Compound Eyes", NULL, 0},
    {"Insomnia", NULL, 0},
    {"Color Change", "Type becomes that of the move that hit it", 0},
    {"Immunity", NULL, 0},
    {"Flash Fire", "Fire immunity", 0},
    {"Shield Dust", NULL, 0},
    {"Own Tempo", NULL, 0},
    {"Suction Cups", NULL, 0},
    {"Intimidate", NULL, 0},
    {"Shadow Tag", NULL, 0},
    {"Rough Skin", NULL, 0},
    {"Wonder Guard", "Only super effective hits", 0},
    {"Levitate", "Ground immunity", 0},
    {"Effect Spore", NULL, 0},
    {"Synchronize", NULL, 0},
    {"Clear Body", NULL, 0},
    {"Natural Cure", NULL, 0},
    {"Lightning Rod", "Electric immunity", 5},
    {"Serene Grace", NULL, 0},
    {"Swift Swim", NULL, 0},
    {"Chlorophyll", NULL, 0},
    {"Illuminate", NULL, 0},
    {"Trace", NULL, 0},
    {"Huge Power", NULL, 0},
    {"Poison Point", NULL, 0},
    {"Inner Focus", NULL, 0},
    {"Magma Armor", NULL, 0},
    {"Water Veil", NULL, 0},
    {"Magnet Pull", NULL, 0},
    {"Soundproof", NULL, 0},
    {"Rain Dish", NULL, 0},
    {"Sand Stream", NULL, 0},
    {"Pressure", NULL, 0},
    {"Thick Fat", "Fire & Ice damage halved", 0},
    {"Early Bird", NULL, 0},
    {"Flame Body", NULL, 0},
    {"Run Away", NULL, 0},
    {"Keen Eye", NULL, 0},
    {"Hyper Cutter", NULL, 0},
    {"Pickup", NULL, 0},
    {"Truant", NULL, 0},
    {"Hustle", NULL, 0},
    {"Cute Charm", NULL, 0},
    {"Plus", NULL, 0},
    {"Minus", NULL, 0},
    {"Forecast", "Type changes with weather", 0},
    {"Sticky Hold", NULL, 0},
    {"Shed Skin", NULL, 0},
    {"Guts", NULL, 0},
    {"Marvel Scale", NULL, 0},
    {"Liquid Ooze", NULL, 0},
    {"Overgrow", NULL, 0},
    {"Blaze", NULL, 0},
    {"Torrent", NULL, 0},
    {"Swarm", NULL, 0},
    {"Rock Head", NULL, 0},
    {"Drought", NULL, 0},
    {"Arena Trap", NULL, 0},
    {"Vital Spirit", NULL, 0},
    {"White Smoke", NULL, 0},
    {"Pure Power", NULL, 0},
    {"Shell Armor", NULL, 0},
    {"Air Lock", NULL, 0},
    {"Tangled Feet", NULL, 0},
    {"Rivalry", NULL, 0},
    {"Magic Guard", NULL, 0},
    {"Dry Skin", "Water immunity; Fire becomes super effective (x1.25)", 0},
    {"Tinted Lens", "Resisted hits do double damage", 0},
    {"Technician", NULL, 0},
    {"Anger Point", NULL, 0},
    {"No Guard", NULL, 0},
    {"Hydration", NULL, 0},
    {"Skill Link", NULL, 0},
    {"Forewarn", NULL, 0},
    {"Reckless", NULL, 0},
    {"Iron Fist", NULL, 0},
    {"Leaf Guard", NULL, 0},
    {"Scrappy", "Normal & Fighting hit Ghost", 0},
    {"Sniper", NULL, 0},
    {"Filter", "Super effective hits -25%", 0},
    {"Mold Breaker", "Ignores target's abilities", 0},
    {"Adaptability", NULL, 0},
    {"Download", NULL, 0},
    {"Solar Power", NULL, 0},
    {"Super Luck", NULL, 0},
    {"Quick Feet", NULL, 0},
    {"Gluttony", NULL, 0},
    {"Snow Cloak", NULL, 0},
    {"Frisk", NULL, 0},
    {"Steadfast", NULL, 0},
    {"Poison Heal", NULL, 0},
    {"Normalize", "All moves become Normal type", 0},
    {"Stall", NULL, 0},
    {"Simple", NULL, 0},
    {"Solid Rock", "Super effective hits -25%", 0},
    {"Anticipation", NULL, 0},
    {"Ice Body", NULL, 0},
    {"Unaware", NULL, 0},
    {"Honey Gather", NULL, 0},
    {"Flower Gift", NULL, 0},
    {"Storm Drain", "Water immunity", 5},
    {"Aftermath", NULL, 0},
    {"Unburden", NULL, 0},
    {"Klutz", NULL, 0},
    {"Heatproof", "Fire damage halved", 0},
    {"Snow Warning", NULL, 0},
    {"Motor Drive", "Electric immunity", 0},
    {"Slow Start", NULL, 0},
    {"Bad Dreams", NULL, 0},
    {"Multitype", "Type matches held Plate", 0},
    {"Big Pecks", NULL, 0},
    {"Unnerve", NULL, 0},
    {"Sand Rush", NULL, 0},
    {"Sheer Force", NULL, 0},
    {"Friend Guard", NULL, 0},
    {"Infiltrator", NULL, 0},
    {"Wonder Skin", NULL, 0},
    {"Sand Force", NULL, 0},
    {"Defiant", NULL, 0},
    {"Justified", NULL, 0},
    {"Regenerator", NULL, 0},
    {"Analytic", NULL, 0},
    {"Poison Touch", NULL, 0},
    {"Overcoat", NULL, 0},
    {"Weak Armor", NULL, 0},
    {"Harvest", NULL, 0},
    {"Healer", NULL, 0},
    {"Moxie", NULL, 0},
    {"Rattled", NULL, 0},
    {"Imposter", NULL, 0},
    {"Multiscale", NULL, 0},
    {"Magic Bounce", NULL, 0},
    {"Sap Sipper", "Grass immunity", 0},
    {"Prankster", NULL, 0},
    {"Telepathy", NULL, 0},
    {"Light Metal", NULL, 0},
    {"Contrary", NULL, 0},
    {"Pickpocket", NULL, 0},
    {"Moody", NULL, 0},
    {"Heavy Metal", NULL, 0},
    {"Toxic Boost", NULL, 0},
    {"Cursed Body", NULL, 0},
    {"Flare Boost", NULL, 0},
    {"Victory Star", NULL, 0},
    {"Zen Mode", NULL, 0},
    {"Mummy", NULL, 0},
    {"Defeatist", NULL, 0},
    {"Illusion", NULL, 0},
    {"Iron Barbs", NULL, 0},
    {"Turboblaze", "Ignores target's abilities", 0},
    {"Teravolt", "Ignores target's abilities", 0},
    {"Competitive", NULL, 0},
    {"Protean", "Type becomes that of the move used", 0},
    {"Bulletproof", NULL, 0},
    {"Magician", NULL, 0},
    {"Cheek Pouch", NULL, 0},
    {"Gale Wings", NULL, 0},
    {"Flower Veil", NULL, 0},
    {"Symbiosis", NULL, 0},
    {"Grass Pelt", NULL, 0},
    {"Fur Coat", NULL, 0},
    {"Stance Change", NULL, 0},
    {"Aroma Veil", NULL, 0},
    {"Sweet Veil", NULL, 0},
    {"Tough Claws", NULL, 0},
    {"Mega Launcher", NULL, 0},
    {"Strong Jaw", NULL, 0},
    {"Refrigerate", "Normal moves become Ice", 0},
    {"Pixilate", "Normal moves become Fairy", 0},
    {"Gooey", NULL, 0},
    {"Fairy Aura", "Fairy moves +33%", 0},
    {"Dark Aura", "Dark moves +33%", 0},
    {"Aura Break", "Reverses Dark/Fairy Aura", 0},
    {"Slush Rush", NULL, 0},
    {"Long Reach", NULL, 0},
    {"Liquid Voice", "Sound moves become Water", 0},
    {"Stakeout", NULL, 0},
    {"Battery", NULL, 0},
    {"Dancer", NULL, 0},
    {"Schooling", NULL, 0},
    {"Merciless", NULL, 0},
    {"Stamina", NULL, 0},
    {"Water Bubble", "Fire damage halved", 0},
    {"Corrosion", NULL, 0},
    {"Fluffy", "Fire damage doubled; 1/2 damage if contact move", 0},
    {"Queenly Majesty", NULL, 0},
    {"Triage", NULL, 0},
    {"Receiver", NULL, 0},
    {"Wimp Out", NULL, 0},
    {"Emergency Exit", NULL, 0},
    {"Water Compaction", NULL, 0},
    {"Innards Out", NULL, 0},
    {"RKS System", "Type matches held Memory", 0},
    {"Shields Down", NULL, 0},
    {"Comatose", NULL, 0},
    {"Disguise", NULL, 0},
    {"Dazzling", NULL, 0},
    {"Berserk", NULL, 0},
    {"Steelworker", "Steel moves +50%", 0},
    {"Electric Surge", NULL, 0},
    {"Psychic Surge", NULL, 0},
    {"Grassy Surge", NULL, 0},
    {"Misty Surge", NULL, 0},
    {"Full Metal Body", NULL, 0},
    {"Shadow Shield", NULL, 0},
    {"Beast Boost", NULL, 0},
    {"Prism Armor", "Super effective hits -25%", 0},
    {"Soul-Heart", NULL, 0},
    {"Neutralizing Gas", NULL, 0},
    {"Libero", "Type becomes that of the move used", 0},
    {"Mirror Armor", NULL, 0},
    {"Cotton Down", NULL, 0},
    {"Ball Fetch", NULL, 0},
    {"Steam Engine", NULL, 0},
    {"Ripen", NULL, 0},
    {"Sand Spit", NULL, 0},
    {"Gulp Missile", NULL, 0},
    {"Propeller Tail", NULL, 0},
    {"Punk Rock", NULL, 0},
    {"Steely Spirit", "Allies' Steel moves +50%", 0},
    {"Perish Body", NULL, 0},
    {"Screen Cleaner", NULL, 0},
    {"Wandering Spirit", NULL, 0},
    {"Ice Scales", NULL, 0},
    {"Power Spot", NULL, 0},
    {"Ice Face", NULL, 0},
    {"Hunger Switch", NULL, 0},
    {"Stalwart", NULL, 0},
    {"Intrepid Sword", NULL, 0},
    {"Dauntless Shield", NULL, 0},
    {"Unseen Fist", NULL, 0},
    {"Transistor", "Electric moves +50%", 0},
    {"Dragon's Maw", "Dragon moves +50%", 0},
    {"Chilling Neigh", NULL, 0},
    {"Grim Neigh", NULL, 0},
    {"Varies with form", NULL, 0}, // shown instead of abilities for Pokemon whose forms differ
    {"Wind Rider", NULL, 0},
    {"Sharpness", NULL, 0},
    {"Lingering Aroma", NULL, 0},
    {"Well-Baked Body", "Fire immunity", 0},
    {"Seed Sower", NULL, 0},
    {"Purifying Salt", "Ghost damage halved", 0},
    {"Electromorphosis", NULL, 0},
    {"Wind Power", NULL, 0},
    {"Guard Dog", NULL, 0},
    {"Mycelium Might", NULL, 0},
    {"Anger Shell", NULL, 0},
    {"Opportunist", NULL, 0},
    {"Rocky Payload", "Rock moves +50%", 0},
    {"Zero to Hero", NULL, 0},
    {"Earth Eater", "Ground immunity", 0},
    {"Toxic Debris", NULL, 0},
    {"Costar", NULL, 0},
    {"Commander", NULL, 0},
    {"Cud Chew", NULL, 0},
    {"Armor Tail", NULL, 0},
    {"Supreme Overlord", NULL, 0},
    {"Protosynthesis", NULL, 0},
    {"Quark Drive", NULL, 0},
    {"Thermal Exchange", NULL, 0},
    {"Good as Gold", NULL, 0},
    {"Tablets of Ruin", NULL, 0},
    {"Sword of Ruin", NULL, 0},
    {"Vessel of Ruin", NULL, 0},
    {"Beads of Ruin", NULL, 0},
    {"Orichalcum Pulse", NULL, 0},
    {"Hadron Engine", NULL, 0},
    {"Supersweet Syrup", NULL, 0},
    {"Hospitality", NULL, 0},
    {"Toxic Chain", NULL, 0},
    {"Tera Shift", NULL, 0},
    {"Poison Puppeteer", NULL, 0},
    // Abilities that only an alternate form has (Mega Evolutions and the like).
    {"Aerilate", "Normal moves become Flying", 0},
    {"As One", NULL, 0},
    {"Aura Guard", NULL, 0},
    {"Battle Bond", NULL, 0},
    {"Curious Medicine", NULL, 0},
    {"Delta Stream", "Flying type's weaknesses removed", 0},
    {"Desolate Land", "Water moves fail", 0},
    {"Dragonize", "Normal moves become Dragon", 0},
    {"Eelevate", "Ground immunity", 0},
    {"Fire Mane", "Fire moves +50%", 0},
    {"Galvanize", "Normal moves become Electric", 0},
    {"Gorilla Tactics", NULL, 0},
    {"Mega Sol", NULL, 0},
    {"Mimicry", "Type changes with terrain", 0},
    {"Mind's Eye", "Normal & Fighting hit Ghost", 0},
    {"Neuroforce", "Super effective hits +25%", 0},
    {"Parental Bond", NULL, 0},
    {"Pastel Veil", NULL, 0},
    {"Piercing Drill", NULL, 0},
    {"Power Construct", NULL, 0},
    {"Power of Alchemy", NULL, 0},
    {"Primordial Sea", "Fire moves fail", 0},
    {"Quick Draw", NULL, 0},
    {"Spicy Spray", NULL, 0},
    {"Surge Surfer", NULL, 0},
    {"Tangling Hair", NULL, 0},
    {"Tera Shell", "All hits not very effective - only w/ full HP", 0},
    {"Teraform Zero", NULL, 0},
};
#define ABILITY_COUNT (sizeof(abilities) / sizeof(abilities[0]))

// The effect text to show for an ability in the given generation, or NULL.
static const char* pt_ability_effect(const PtAbility* a, int generation) {
    return (a->effect && generation >= a->effect_from_gen) ? a->effect : NULL;
}

// Generation 1 (Red / Blue / Yellow) type chart.
// Rows = attacking type, columns = defending type.
// Values are the multiplier x4 (0 = immune, 2 = 0.5x, 4 = 1x, 8 = 2x),
// so plain integer math can be used to combine dual defensive types.
//
// Compared with the modern chart, only four matchups differ:
//   Ice    -> Fire    : 1x    (later 0.5x)
//   Poison -> Bug     : 2x    (later 1x)
//   Bug    -> Poison  : 2x    (later 0.5x)
//   Ghost  -> Psychic : 0x    (later 2x; a well-known Gen 1 bug)
// Rows/columns 15-17 (Dark, Steel, Fairy) don't exist in Generation 1. They are
// filled with neutral (4) values purely so the table keeps the same shape as
// the charts for later generations; the app never reads them.
// Source: the game's own matchup table (pret/pokered, data/types/type_matchups.asm).
static const uint8_t type_chart_gen1[TYPE_COUNT][TYPE_COUNT] = {
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 0, 4, 4, 4, 4}, // Normal
    {4, 2, 2, 4, 8, 8, 4, 4, 4, 4, 4, 8, 2, 4, 2, 4, 4, 4}, // Fire
    {4, 8, 2, 4, 2, 4, 4, 4, 8, 4, 4, 4, 8, 4, 2, 4, 4, 4}, // Water
    {4, 4, 8, 2, 2, 4, 4, 4, 0, 8, 4, 4, 4, 4, 2, 4, 4, 4}, // Electric
    {4, 2, 8, 4, 2, 4, 4, 2, 8, 2, 4, 2, 8, 4, 2, 4, 4, 4}, // Grass
    {4, 4, 2, 4, 8, 2, 4, 4, 8, 8, 4, 4, 4, 4, 8, 4, 4, 4}, // Ice
    {8, 4, 4, 4, 4, 8, 4, 2, 4, 2, 2, 2, 8, 0, 4, 4, 4, 4}, // Fighting
    {4, 4, 4, 4, 8, 4, 4, 2, 2, 4, 4, 8, 2, 2, 4, 4, 4, 4}, // Poison
    {4, 8, 4, 8, 2, 4, 4, 8, 4, 0, 4, 2, 8, 4, 4, 4, 4, 4}, // Ground
    {4, 4, 4, 2, 8, 4, 8, 4, 4, 4, 4, 8, 2, 4, 4, 4, 4, 4}, // Flying
    {4, 4, 4, 4, 4, 4, 8, 8, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4}, // Psychic
    {4, 2, 4, 4, 8, 4, 2, 8, 4, 2, 8, 4, 4, 2, 4, 4, 4, 4}, // Bug
    {4, 8, 4, 4, 4, 8, 2, 4, 2, 8, 4, 8, 4, 4, 4, 4, 4, 4}, // Rock
    {0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 0, 4, 4, 8, 4, 4, 4, 4}, // Ghost
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 4}, // Dragon
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, // Dark (n/a)
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, // Steel (n/a)
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, // Fairy (n/a)
};

// Generation 2 (Gold / Silver / Crystal) type chart, which Generations 3, 4 and 5
// reuse unchanged. Same layout as above.
// Dark and Steel now exist, and the four Generation 1 oddities are fixed:
//   Ice    -> Fire    : 0.5x   Poison -> Bug : 1x
//   Bug    -> Poison  : 0.5x   Ghost -> Psychic : 2x
// Generations 2-5 also differ from today's chart in one way that isn't
// visible from Gen 1: Steel resists Ghost and Dark (removed in Gen 6).
// Fairy (column/row 17) doesn't exist yet, so it is neutral filler.
// Source: the game's own matchup table (pret/pokecrystal, data/types/type_matchups.asm).
static const uint8_t type_chart_gen2_5[TYPE_COUNT][TYPE_COUNT] = {
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 0, 4, 4, 2, 4}, // Normal
    {4, 2, 2, 4, 8, 8, 4, 4, 4, 4, 4, 8, 2, 4, 2, 4, 8, 4}, // Fire
    {4, 8, 2, 4, 2, 4, 4, 4, 8, 4, 4, 4, 8, 4, 2, 4, 4, 4}, // Water
    {4, 4, 8, 2, 2, 4, 4, 4, 0, 8, 4, 4, 4, 4, 2, 4, 4, 4}, // Electric
    {4, 2, 8, 4, 2, 4, 4, 2, 8, 2, 4, 2, 8, 4, 2, 4, 2, 4}, // Grass
    {4, 2, 2, 4, 8, 2, 4, 4, 8, 8, 4, 4, 4, 4, 8, 4, 2, 4}, // Ice
    {8, 4, 4, 4, 4, 8, 4, 2, 4, 2, 2, 2, 8, 0, 4, 8, 8, 4}, // Fighting
    {4, 4, 4, 4, 8, 4, 4, 2, 2, 4, 4, 4, 2, 2, 4, 4, 0, 4}, // Poison
    {4, 8, 4, 8, 2, 4, 4, 8, 4, 0, 4, 2, 8, 4, 4, 4, 8, 4}, // Ground
    {4, 4, 4, 2, 8, 4, 8, 4, 4, 4, 4, 8, 2, 4, 4, 4, 2, 4}, // Flying
    {4, 4, 4, 4, 4, 4, 8, 8, 4, 4, 2, 4, 4, 4, 4, 0, 2, 4}, // Psychic
    {4, 2, 4, 4, 8, 4, 2, 2, 4, 2, 8, 4, 4, 2, 4, 8, 2, 4}, // Bug
    {4, 8, 4, 4, 4, 8, 2, 4, 2, 8, 4, 8, 4, 4, 4, 4, 2, 4}, // Rock
    {0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 8, 4, 2, 2, 4}, // Ghost
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 2, 4}, // Dragon
    {4, 4, 4, 4, 4, 4, 2, 4, 4, 4, 8, 4, 4, 8, 4, 2, 2, 4}, // Dark
    {4, 2, 2, 2, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 2, 4}, // Steel
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, // Fairy
};

// Generation 6 (X / Y / Omega Ruby / Alpha Sapphire) type chart, which Generation 7
// and later games reuse. Same layout as above, now with all 18 types.
// Two changes from Generations 2-5, both about the new Fairy type:
//   - Fairy exists: it is immune to Dragon, weak to Poison and Steel, and
//     resists Fighting, Bug and Dark.
//   - Steel no longer resists Ghost and Dark.
// Source: the games' matchup table as published in PokeAPI (data/v2/csv/type_efficacy.csv).
static const uint8_t type_chart_gen6_plus[TYPE_COUNT][TYPE_COUNT] = {
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 0, 4, 4, 2, 4}, // Normal
    {4, 2, 2, 4, 8, 8, 4, 4, 4, 4, 4, 8, 2, 4, 2, 4, 8, 4}, // Fire
    {4, 8, 2, 4, 2, 4, 4, 4, 8, 4, 4, 4, 8, 4, 2, 4, 4, 4}, // Water
    {4, 4, 8, 2, 2, 4, 4, 4, 0, 8, 4, 4, 4, 4, 2, 4, 4, 4}, // Electric
    {4, 2, 8, 4, 2, 4, 4, 2, 8, 2, 4, 2, 8, 4, 2, 4, 2, 4}, // Grass
    {4, 2, 2, 4, 8, 2, 4, 4, 8, 8, 4, 4, 4, 4, 8, 4, 2, 4}, // Ice
    {8, 4, 4, 4, 4, 8, 4, 2, 4, 2, 2, 2, 8, 0, 4, 8, 8, 2}, // Fighting
    {4, 4, 4, 4, 8, 4, 4, 2, 2, 4, 4, 4, 2, 2, 4, 4, 0, 8}, // Poison
    {4, 8, 4, 8, 2, 4, 4, 8, 4, 0, 4, 2, 8, 4, 4, 4, 8, 4}, // Ground
    {4, 4, 4, 2, 8, 4, 8, 4, 4, 4, 4, 8, 2, 4, 4, 4, 2, 4}, // Flying
    {4, 4, 4, 4, 4, 4, 8, 8, 4, 4, 2, 4, 4, 4, 4, 0, 2, 4}, // Psychic
    {4, 2, 4, 4, 8, 4, 2, 2, 4, 2, 8, 4, 4, 2, 4, 8, 2, 2}, // Bug
    {4, 8, 4, 4, 4, 8, 2, 4, 2, 8, 4, 8, 4, 4, 4, 4, 2, 4}, // Rock
    {0, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 8, 4, 2, 4, 4}, // Ghost
    {4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 4, 2, 0}, // Dragon
    {4, 4, 4, 4, 4, 4, 2, 4, 4, 4, 8, 4, 4, 8, 4, 2, 4, 2}, // Dark
    {4, 2, 2, 2, 4, 8, 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 2, 8}, // Steel
    {4, 2, 4, 4, 4, 4, 8, 2, 4, 4, 4, 4, 4, 4, 8, 8, 2, 4}, // Fairy
};

// How "Pokedex Browse" groups each generation's list. A generation with one
// group skips the group list and opens straight onto the Pokemon.
static const PtGroup groups_gen1[] = {
    {"#001-#151  Kanto", 1, 151},
};
static const PtGroup groups_gen2[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
};
static const PtGroup groups_gen3[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
};
static const PtGroup groups_gen4[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
};
static const PtGroup groups_gen7[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
    {"#494-#649  Unova", 494, 649},
    {"#650-#721  Kalos", 650, 721},
    {"#722-#807  Alola", 722, 807},
};
static const PtGroup groups_gen8[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
    {"#494-#649  Unova", 494, 649},
    {"#650-#721  Kalos", 650, 721},
    {"#722-#807  Alola", 722, 807},
    {"#808-#809  Meltan", 808, 809},
    {"#810-#898  Galar", 810, 898},
    {"#899-#905  Hisui", 899, 905},
};
static const PtGroup groups_gen9[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
    {"#494-#649  Unova", 494, 649},
    {"#650-#721  Kalos", 650, 721},
    {"#722-#807  Alola", 722, 807},
    {"#808-#809  Meltan", 808, 809},
    {"#810-#898  Galar", 810, 898},
    {"#899-#905  Hisui", 899, 905},
    {"#906-#1025  Paldea", 906, 1025},
};
static const PtGroup groups_gen6[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
    {"#494-#649  Unova", 494, 649},
    {"#650-#721  Kalos", 650, 721},
};
static const PtGroup groups_gen5[] = {
    {"#001-#151  Kanto", 1, 151},
    {"#152-#251  Johto", 152, 251},
    {"#252-#386  Hoenn", 252, 386},
    {"#387-#493  Sinnoh", 387, 493},
    {"#494-#649  Unova", 494, 649},
};

// The generations the app knows about. The "Choose Generation" list is built
// from this table, in this order.
static const GenerationInfo generations[] = {
    {
        .menu_label = "Generation 1",
        .dialog_title = "Generation 1 Games",
        .games = "PKMN Red (GB)\nPKMN Blue (GB)\nPKMN Yellow (GB)",
        .type_count = 15, // Normal ... Dragon
        .chart = type_chart_gen1,
        .dex_count = 151,
        .evo_pool_size = 1317, // exact need is 1301 bytes; +16 spare
        .groups = groups_gen1,
        .group_count = sizeof(groups_gen1) / sizeof(groups_gen1[0]),
        .csv_name = "pokedex_gen1.csv",
    },
    {
        .menu_label = "Generation 2",
        .dialog_title = "Generation 2 Games",
        .games = "PKMN Gold (GBC)\nPKMN Silver (GBC)\nPKMN Crystal (GBC)",
        .type_count = 17, // Normal ... Steel (no Fairy yet)
        .chart = type_chart_gen2_5,
        .dex_count = 251,
        .evo_pool_size = 2268, // exact need is 2252 bytes; +16 spare
        .groups = groups_gen2,
        .group_count = sizeof(groups_gen2) / sizeof(groups_gen2[0]),
        .csv_name = "pokedex_gen2.csv",
    },
    {
        .menu_label = "Generation 3",
        .dialog_title = "Generation 3 Games",
        .games = "PKMN Ruby (GBA)\nPKMN Sapphire (GBA)\nPKMN Emerald (GBA)\nPKMN FireRed (GBA)\nPKMN LeafGreen (GBA)",
        .type_count = 17, // same 17 types as Generation 2
        .chart = type_chart_gen2_5, // and the same matchups
        .dex_count = 386,
        .evo_pool_size = 3434, // exact need is 3418 bytes; +16 spare
        .groups = groups_gen3,
        .group_count = sizeof(groups_gen3) / sizeof(groups_gen3[0]),
        .csv_name = "pokedex_gen3.csv",
    },
    {
        .menu_label = "Generation 4",
        .dialog_title = "Generation 4 Games",
        .games = "PKMN Diamond (DS)\nPKMN Pearl (DS)\nPKMN Platinum (DS)\nPKMN HeartGold (DS)\nPKMN SoulSilver (DS)",
        .type_count = 17, // same 17 types as Generation 2
        .chart = type_chart_gen2_5, // and the same matchups
        .dex_count = 493,
        .evo_pool_size = 5201, // exact need is 5185 bytes; +16 spare
        .groups = groups_gen4,
        .group_count = sizeof(groups_gen4) / sizeof(groups_gen4[0]),
        .csv_name = "pokedex_gen4.csv",
    },
    {
        .menu_label = "Generation 5",
        .dialog_title = "Generation 5 Games",
        .games = "PKMN Black (DS)\nPKMN White (DS)\nPKMN Black 2 (DS)\nPKMN White 2 (DS)",
        .type_count = 17, // same 17 types as Generation 2
        .chart = type_chart_gen2_5, // and the same matchups
        .dex_count = 649,
        .evo_pool_size = 6922, // exact need is 6906 bytes; +16 spare
        .groups = groups_gen5,
        .group_count = sizeof(groups_gen5) / sizeof(groups_gen5[0]),
        .csv_name = "pokedex_gen5.csv",
    },
    {
        .menu_label = "Generation 6",
        .dialog_title = "Generation 6 Games",
        .games = "PKMN X (3DS)\nPKMN Y (3DS)\nPKMN Omega Ruby (3DS)\nPKMN Alpha Sapphire (3DS)",
        .type_count = 18, // Normal ... Fairy
        .chart = type_chart_gen6_plus,
        .dex_count = 721,
        .evo_pool_size = 9190, // exact need is 9174 bytes; +16 spare
        .groups = groups_gen6,
        .group_count = sizeof(groups_gen6) / sizeof(groups_gen6[0]),
        .csv_name = "pokedex_gen6.csv",
    },
    {
        .menu_label = "Generation 7",
        .dialog_title = "Generation 7 Games",
        .games = "PKMN Sun (3DS)\nPKMN Moon (3DS)\nPKMN Ultra Sun (3DS)\nPKMN Ultra Moon (3DS)",
        .type_count = 18, // same 18 types as Generation 6
        .chart = type_chart_gen6_plus, // and the same matchups
        .dex_count = 807, // #808-#809 (Meltan, Melmetal) are not in the 3DS games
        .evo_pool_size = 11357, // exact need is 11341 bytes; +16 spare
        .groups = groups_gen7,
        .group_count = sizeof(groups_gen7) / sizeof(groups_gen7[0]),
        .csv_name = "pokedex_gen7.csv",
    },
    {
        .menu_label = "Generation 8",
        .dialog_title = "Generation 8 Games",
        .games = "PKMN Sword (NS)\nPKMN Shield (NS)\nPKMN Brilliant Diamond (NS)\nPKMN Shining Pearl (NS)\nPKMN Legends: Arceus (NS)",
        .type_count = 18, // same 18 types as Generation 6
        .chart = type_chart_gen6_plus, // and the same matchups
        .dex_count = 905, // #1-#905, including Meltan and Melmetal
        .evo_pool_size = 12949, // exact need is 12933 bytes; +16 spare
        .groups = groups_gen8,
        .group_count = sizeof(groups_gen8) / sizeof(groups_gen8[0]),
        .csv_name = "pokedex_gen8.csv",
    },
    {
        .menu_label = "Generation 9",
        .dialog_title = "Generation 9 Games",
        .games = "PKMN Scarlet (NS)\nPKMN Violet (NS)\nPKMN Legends: Z-A (NS)",
        .type_count = 18, // same 18 types as Generation 6
        .chart = type_chart_gen6_plus, // and the same matchups (Terastal types do not change the chart)
        .dex_count = 1025, // #1-#1025
        .evo_pool_size = 17084, // exact need is 17068 bytes; +16 spare
        .groups = groups_gen9,
        .group_count = sizeof(groups_gen9) / sizeof(groups_gen9[0]),
        .csv_name = "pokedex_gen9.csv",
    },
};
#define GENERATION_COUNT (sizeof(generations) / sizeof(generations[0]))

// --- Forward declarations -------------------------------------------------

static void pt_populate_gen_select(PokemonTypesApp* app);
static void pt_show_gen_confirm(PokemonTypesApp* app);
static void pt_show_title(PokemonTypesApp* app);
static void pt_confirm_generation(PokemonTypesApp* app);
static void pt_populate_main_menu(PokemonTypesApp* app);
static void pt_populate_type_attack(PokemonTypesApp* app);
static void pt_populate_type_defense1(PokemonTypesApp* app);
static void pt_populate_type_defense2(PokemonTypesApp* app);
static void pt_show_type_result(PokemonTypesApp* app);
static void pt_populate_defense_options(PokemonTypesApp* app);
static void pt_populate_defender_source(PokemonTypesApp* app);
static void pt_defender_picked(PokemonTypesApp* app);
static void pt_show_pokedex_search(PokemonTypesApp* app);
static void pt_populate_pokedex_results(PokemonTypesApp* app);
static void pt_populate_pokedex_groups(PokemonTypesApp* app);
static void pt_show_pokedex_detail(PokemonTypesApp* app);
static void pt_show_pokedex_error(PokemonTypesApp* app);
static void pt_submenu_callback(void* context, uint32_t index);

// --- Pokedex loading & search ----------------------------------------------

// Splits a comma-separated line IN PLACE: each comma is overwritten with a
// '\0', and out[i] points at the start of field i. Replaces strtok(), which
// the Flipper firmware's API exposes but has disabled. Unlike strtok it also
// keeps empty fields (",,") as empty strings instead of skipping them.
// Returns how many fields were found (at most max_fields).
static int pt_split_csv(char* line, char** out, int max_fields) {
    int count = 0;
    char* p = line;

    while(count < max_fields) {
        out[count++] = p;

        while(*p != ',' && *p != '\0') {
            p++;
        }
        if(*p == '\0') break; // last field reached

        *p = '\0'; // terminate this field
        p++; // step over the comma to the next field
    }

    return count;
}

// Frees the Pokedex data and forgets the confirmed generation. Called when you
// back out to "Choose Generation" and when the app exits.
static void pt_unload_generation(PokemonTypesApp* app) {
    free(app->pokedex);
    free(app->filtered_indices);
    free(app->evo_pool);
    app->evo_pool = NULL;
    app->evo_pool_capacity = 0;
    app->evo_pool_used = 0;
    app->pokedex = NULL;
    app->filtered_indices = NULL;
    app->pokedex_capacity = 0;
    app->pokedex_count = 0;
    app->filtered_count = 0;
    app->selected_position = 0;
    app->browse_mode = false;
    app->group_choice = 0;
    app->list_title[0] = '\0';
    app->browse_kind = 0;
    app->browse_arg = 0;
    app->browse_letter = 'A';
    app->matchup_view = false;
    app->detail_form = -1;
    app->detail_explicit = false;
    app->pokedex_loaded = false;
    app->search_buffer[0] = '\0';
    app->pick_mode = false;
    app->ignore_abilities = false;
    app->defender_is_pokemon = false;
    app->gen = NULL;
}

// Finds an ability by name in abilities[]. Returns 0 ("no ability") for "-",
// for a missing column, or for a name that isn't in the table.
static uint16_t pt_ability_index(const char* name) {
    if(name == NULL || strcmp(name, "-") == 0) return 0;
    for(size_t i = 1; i < ABILITY_COUNT; i++) {
        if(strcmp(abilities[i].name, name) == 0) return (uint16_t)i;
    }
    return 0;
}

// --- Forms --------------------------------------------------------------------
//
// A Pokemon with alternate forms (Mega, Alolan, Rotom's appliances...) has a
// FormData text in the CSV: one token per form, joined by '|'. A token is
//     Label:TT:Ability/Ability/Ability^:Note
// TT are the form's types as letters (A = first entry of type_names), and an
// ability ending in '^' is the Hidden Ability. Everything after the label is optional:
//   no types      -> the Pokemon's own types
//   no abilities  -> the Pokemon's own abilities ("?" = not known)
//   Note          -> how the form is obtained or what it evolves into (may contain ':')
// The first token is the Pokemon's default form.

// Copies the next 'sep'-separated piece of *cursor into out and advances *cursor
// past it (to NULL after the last piece). Returns false when nothing is left.
static bool pt_next_part_sep(const char** cursor, char* out, size_t out_size, char sep) {
    if(*cursor == NULL) return false;

    const char* p = *cursor;
    size_t n = 0;
    while(*p != '\0' && *p != sep) {
        if(n + 1 < out_size) out[n++] = *p;
        p++;
    }
    out[n] = '\0';

    *cursor = (*p == sep) ? p + 1 : NULL;
    return true;
}

static bool pt_next_part(const char** cursor, char* out, size_t out_size) {
    return pt_next_part_sep(cursor, out, out_size, '/');
}

// A list of Pokemon (filtered_indices) can hold a plain Pokemon, or one particular
// form of a Pokemon (listed by Browse by Type / by Ability). The form number + 1
// sits above the 16 bits of the entry number; 0 there means "no particular form".
#define PT_FI_ENTRY(v) ((v) & 0xFFFF)
#define PT_FI_FORM(v) (((v) >> 16) - 1)
#define PT_FI_MAKE(entry, form) ((entry) | (((form) + 1) << 16))

// Reads one form token. Anything the token leaves out is flagged inherit_* (see
// pt_resolve_form); a token with no types and no abilities IS the Pokemon itself.
static void pt_parse_form(const char* token, PtDefender* d) {
    d->label[0] = '\0';
    d->t1 = -1;
    d->t2 = -1;
    d->ab_count = 0;
    d->ab_unknown = false;
    d->inherit_types = true;
    d->inherit_ab = true;
    d->note[0] = '\0';

    const char* c1 = strchr(token, ':');
    size_t label_len = c1 ? (size_t)(c1 - token) : strlen(token);
    if(label_len >= sizeof(d->label)) label_len = sizeof(d->label) - 1;
    memcpy(d->label, token, label_len);
    d->label[label_len] = '\0';
    if(!c1) return;

    const char* types = c1 + 1;
    const char* c2 = strchr(types, ':');
    const char* types_end = c2 ? c2 : types + strlen(types);
    int type_n = 0;
    for(const char* q = types; q < types_end; q++) {
        int idx = *q - 'A';
        if(idx < 0 || idx >= TYPE_COUNT) continue;
        if(type_n == 0) d->t1 = idx;
        if(type_n == 1) d->t2 = idx;
        type_n++;
    }
    d->inherit_types = (type_n == 0);
    if(!c2) return;

    const char* a = c2 + 1;
    const char* c3 = strchr(a, ':');
    const char* a_end = c3 ? c3 : a + strlen(a);
    if(c3) snprintf(d->note, sizeof(d->note), "%s", c3 + 1);
    if(a_end == a) return;
    d->inherit_ab = false;
    if(a_end - a == 1 && *a == '?') {
        d->ab_unknown = true;
        return;
    }
    while(a < a_end && d->ab_count < 3) {
        const char* end = a;
        while(end < a_end && *end != '/') end++;
        size_t len = (size_t)(end - a);
        bool hidden = false;
        if(len > 0 && a[len - 1] == '^') {
            hidden = true;
            len--;
        }
        if(len > 0) {
            if(len >= sizeof(d->ab[0])) len = sizeof(d->ab[0]) - 1;
            memcpy(d->ab[d->ab_count], a, len);
            d->ab[d->ab_count][len] = '\0';
            d->ab_hidden[d->ab_count] = hidden;
            d->ab_count++;
        }
        a = (end < a_end) ? end + 1 : a_end;
    }
}

// The entry's own abilities.
static void pt_entry_abilities(const PokemonEntry* e, PtDefender* d) {
    d->ab_count = 0;
    for(int i = 0; i < 3; i++) {
        if(e->ability[i] == 0) continue;
        const char* name = abilities[e->ability[i]].name;
        bool dup = false;
        for(int k = 0; k < d->ab_count; k++) {
            if(strcmp(d->ab[k], name) == 0) dup = true;
        }
        if(dup) continue;
        snprintf(d->ab[d->ab_count], sizeof(d->ab[0]), "%s", name);
        d->ab_hidden[d->ab_count] = (i == 2);
        d->ab_count++;
    }
}

// The entry's own (default) types and abilities.
static void pt_defender_from_entry(const PokemonEntry* e, PtDefender* d) {
    d->label[0] = '\0';
    d->t1 = e->type1;
    d->t2 = (e->type2 == PT_NO_TYPE) ? -1 : e->type2;
    d->ab_unknown = false;
    d->inherit_types = false;
    d->inherit_ab = false;
    d->note[0] = '\0';
    pt_entry_abilities(e, d);
}

// Fills in whatever a parsed form token inherits from its Pokemon.
static void pt_resolve_form(const PokemonEntry* e, PtDefender* d) {
    if(d->inherit_types) {
        d->t1 = e->type1;
        d->t2 = (e->type2 == PT_NO_TYPE) ? -1 : e->type2;
        d->inherit_types = false;
    }
    if(d->inherit_ab) {
        pt_entry_abilities(e, d);
        d->inherit_ab = false;
    }
}

static const char* pt_entry_formdata(const PokemonTypesApp* app, const PokemonEntry* e) {
    return e->formdata_offset ? app->evo_pool + e->formdata_offset : NULL;
}

// Reads form number 'index' of a Pokemon, fully resolved. False if there is no such form.
#define PT_TOKEN_MAX 128
static bool pt_form_get(const PokemonTypesApp* app, const PokemonEntry* e, int index, PtDefender* d) {
    const char* cursor = pt_entry_formdata(app, e);
    char token[PT_TOKEN_MAX];
    int i = 0;
    while(pt_next_part_sep(&cursor, token, sizeof(token), '|')) {
        if(i++ != index) continue;
        pt_parse_form(token, d);
        pt_resolve_form(e, d);
        snprintf(d->name, sizeof(d->name), "%s", e->name);
        return true;
    }
    return false;
}

// Reads the generation's CSV from the SD card into memory.
static bool pokedex_load(PokemonTypesApp* app, const GenerationInfo* gen) {
    app->pokedex_capacity = gen->dex_count;
    app->pokedex = malloc(sizeof(PokemonEntry) * app->pokedex_capacity);
    app->filtered_indices = malloc(sizeof(int) * app->pokedex_capacity);
    app->evo_pool_capacity = gen->evo_pool_size;
    app->evo_pool = malloc(app->evo_pool_capacity);
    // The first 4 bytes are the shared "no evolution" text: "-\0-\0" (offset 0).
    memcpy(app->evo_pool, "-\0-\0", 4);
    app->evo_pool_used = 4;
    app->pokedex_count = 0;
    app->filtered_count = 0;

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", POKEDEX_DATA_DIR, gen->csv_name);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, POKEDEX_DATA_DIR);

    Stream* stream = file_stream_alloc(storage);
    bool ok = file_stream_open(stream, path, FSAM_READ, FSOM_OPEN_EXISTING);

    if(ok) {
        FuriString* line = furi_string_alloc();
        bool first_line = true;

        while(stream_read_line(stream, line) && app->pokedex_count < app->pokedex_capacity) {
            if(first_line) {
                first_line = false;
                continue; // skip CSV header row
            }

            char buf[384];
            strncpy(buf, furi_string_get_cstr(line), sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';

            size_t len = strlen(buf);
            while(len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
                buf[--len] = '\0';
            }
            if(len == 0) continue;

            // Columns: Number,Name,Type1,Type2,EvolvesTo,EvoMethod,Ability1,Ability2,HiddenAbility,Forms,FormData
            // (the ability columns are missing from the Generation 1-2 files, the Forms
            // column is no longer used, and FormData is only filled in for Pokemon with forms).
            char* fields[11] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
            int field_count = pt_split_csv(buf, fields, 11);

            char* tok_number = field_count > 0 ? fields[0] : NULL;
            char* tok_name = field_count > 1 ? fields[1] : NULL;
            char* tok_type1 = field_count > 2 ? fields[2] : NULL;
            char* tok_type2 = field_count > 3 ? fields[3] : NULL;
            char* tok_evolves = field_count > 4 ? fields[4] : NULL;
            char* tok_method = field_count > 5 ? fields[5] : NULL;
            char* tok_ability1 = field_count > 6 ? fields[6] : NULL;
            char* tok_ability2 = field_count > 7 ? fields[7] : NULL;
            char* tok_hidden = field_count > 8 ? fields[8] : NULL;
            char* tok_formdata = field_count > 10 ? fields[10] : NULL;

            if(!tok_number || !tok_name || !tok_type1) continue;

            PokemonEntry* entry = &app->pokedex[app->pokedex_count];
            entry->number = atoi(tok_number);

            strncpy(entry->name, tok_name, sizeof(entry->name) - 1);
            entry->name[sizeof(entry->name) - 1] = '\0';

            entry->type1 = pt_type_index(tok_type1);
            entry->type2 = tok_type2 ? pt_type_index(tok_type2) : PT_NO_TYPE;
            if(entry->type1 == PT_NO_TYPE) continue; // unknown first type: skip the row

            // Evolution text goes into the shared pool (unless there is none, or
            // the pool is somehow full: then the entry just shows "Does not evolve").
            const char* evo_to = tok_evolves ? tok_evolves : "-";
            const char* evo_how = tok_method ? tok_method : "-";
            entry->evo_offset = 0;
            if(strcmp(evo_to, "-") != 0) {
                int need = (int)strlen(evo_to) + 1 + (int)strlen(evo_how) + 1;
                if(app->evo_pool_used + need <= app->evo_pool_capacity) {
                    char* dest = app->evo_pool + app->evo_pool_used;
                    memcpy(dest, evo_to, strlen(evo_to) + 1);
                    memcpy(dest + strlen(evo_to) + 1, evo_how, strlen(evo_how) + 1);
                    entry->evo_offset = (uint16_t)app->evo_pool_used;
                    app->evo_pool_used += need;
                }
            }

            // FormData (the forms of this Pokemon) goes into the pool too (offset 0 = none).
            entry->formdata_offset = 0;
            if(tok_formdata && tok_formdata[0] != '\0' && strcmp(tok_formdata, "-") != 0) {
                int need = (int)strlen(tok_formdata) + 1;
                if(app->evo_pool_used + need <= app->evo_pool_capacity) {
                    memcpy(app->evo_pool + app->evo_pool_used, tok_formdata, (size_t)need);
                    entry->formdata_offset = (uint16_t)app->evo_pool_used;
                    app->evo_pool_used += need;
                }
            }

            entry->ability[0] = pt_ability_index(tok_ability1);
            entry->ability[1] = pt_ability_index(tok_ability2);
            entry->ability[2] = pt_ability_index(tok_hidden);

            app->pokedex_count++;
        }

        furi_string_free(line);
        file_stream_close(stream);
    }

    stream_free(stream);
    furi_record_close(RECORD_STORAGE);

    app->pokedex_loaded = ok && app->pokedex_count > 0;
    return app->pokedex_loaded;
}

static bool pt_str_contains_ci(const char* haystack, const char* needle) {
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if(nlen == 0) return true;
    if(nlen > hlen) return false;

    for(size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        for(; j < nlen; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if(a >= 'A' && a <= 'Z') a += 32;
            if(b >= 'A' && b <= 'Z') b += 32;
            if(a != b) break;
        }
        if(j == nlen) return true;
    }
    return false;
}

// Search: every entry whose name contains the typed text.
static void pokedex_filter(PokemonTypesApp* app) {
    app->filtered_count = 0;
    bool empty_query = (app->search_buffer[0] == '\0');

    for(int i = 0; i < app->pokedex_count; i++) {
        bool match = empty_query || pt_str_contains_ci(app->pokedex[i].name, app->search_buffer);
        if(!match) continue;
        app->filtered_indices[app->filtered_count++] = i;
    }
}

static void pokedex_filter_group(PokemonTypesApp* app, const PtGroup* group) {
    app->filtered_count = 0;

    for(int i = 0; i < app->pokedex_count; i++) {
        int n = app->pokedex[i].number;
        if(n >= group->first && n <= group->last) {
            app->filtered_indices[app->filtered_count++] = i;
        }
    }
}

static void pt_filter_add(PokemonTypesApp* app, int entry, int form) {
    if(app->filtered_count < app->pokedex_capacity) {
        app->filtered_indices[app->filtered_count++] = PT_FI_MAKE(entry, form);
    }
}

// Does this (resolved) form have the type, or the ability (an index into abilities[])?
static bool pt_form_matches(const PtDefender* d, int kind, int arg) {
    if(kind == 1) return d->t1 == arg || d->t2 == arg;
    for(int i = 0; i < d->ab_count; i++) {
        if(pt_ability_index(d->ab[i]) == arg) return true;
    }
    return false;
}

// Browse by Type (kind 1) or by Ability (kind 2): every Pokemon that has it. A
// form is listed on its own only when its Pokemon's default form does not have it
// (Mega Charizard X shows up under Dragon, Mega Charizard Y does not show up
// under Fire again). A Pokemon with no plain default form (Basculin, Ogerpon...)
// is listed as its forms.
static void pokedex_filter_attr(PokemonTypesApp* app, int kind, int arg) {
    app->filtered_count = 0;
    char token[PT_TOKEN_MAX];
    for(int i = 0; i < app->pokedex_count; i++) {
        const PokemonEntry* e = &app->pokedex[i];
        const char* formdata = pt_entry_formdata(app, e);
        PtDefender d;
        if(formdata == NULL) {
            pt_defender_from_entry(e, &d);
            if(pt_form_matches(&d, kind, arg)) pt_filter_add(app, i, -1);
            continue;
        }
        const char* cursor = formdata;
        int k = 0;
        bool default_listed = false; // the plain Pokemon itself was listed
        while(pt_next_part_sep(&cursor, token, sizeof(token), '|')) {
            pt_parse_form(token, &d);
            bool plain = d.inherit_types && d.inherit_ab; // this token is the Pokemon itself
            pt_resolve_form(e, &d);
            bool match = pt_form_matches(&d, kind, arg);
            if(k == 0 && plain) {
                if(match) {
                    pt_filter_add(app, i, -1);
                    default_listed = true;
                }
            } else if(match && !default_listed) {
                pt_filter_add(app, i, k);
            }
            k++;
        }
    }
}

// --- Generation screens -------------------------------------------------

static void pt_populate_gen_select(PokemonTypesApp* app) {
    pt_unload_generation(app); // leaving a generation frees its data

    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Choose Generation");
    for(size_t i = 0; i < GENERATION_COUNT; i++) {
        submenu_add_item(app->submenu, generations[i].menu_label, (uint32_t)i, pt_submenu_callback, app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)app->gen_choice);

    app->step = StepGenSelect;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// Back / Confirm buttons on the "Generation N Games" screen. A widget button
// reports every kind of press (press, release, long press...), so only the
// short press is acted on.
// --- Memory: guard and readout ---------------------------------------------
//
// Loading a generation allocates three blocks from the Flipper's heap (the
// Pokedex entries, the list of search results and the evolution/forms text).
// If there is not enough room the firmware would stop the app with a crash
// screen, so we work out the need first and say so politely instead.

// Heap kept free for the screens themselves (a list of up to 150 results costs
// about 9 KB while it is showing).
#define PT_MEM_MARGIN (12 * 1024)

// Bytes a generation's data needs in total, and the size of its biggest single
// block (an allocation needs one unbroken piece of heap, not just free bytes).
static size_t pt_memory_needed(const GenerationInfo* g, size_t* biggest) {
    size_t entries = sizeof(PokemonEntry) * (size_t)g->dex_count;
    size_t indexes = sizeof(int) * (size_t)g->dex_count;
    size_t text = (size_t)g->evo_pool_size;
    if(biggest) *biggest = entries > text ? entries : text;
    return entries + indexes + text;
}

static bool pt_memory_ok(const GenerationInfo* g) {
    size_t biggest;
    size_t total = pt_memory_needed(g, &biggest);
    return memmgr_get_free_heap() >= total + PT_MEM_MARGIN &&
           memmgr_heap_get_max_free_block() >= biggest + 1024;
}

// Shows a title and a few lines of text on the plain widget screen.
static void pt_show_info(PokemonTypesApp* app, PtStep step, const char* title, const char* body) {
    widget_reset(app->widget);
    widget_add_string_element(app->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, title);
    widget_add_string_multiline_element(
        app->widget, 64, 38, AlignCenter, AlignCenter, FontSecondary, body);
    app->step = step;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewResult);
}

#define PT_KB(bytes) ((unsigned)(((bytes) + 512) / 1024))

// From the confirm screen: what this generation needs against what is free now.
static void pt_show_gen_mem(PokemonTypesApp* app) {
    const GenerationInfo* g = &generations[app->gen_choice];
    size_t biggest;
    size_t total = pt_memory_needed(g, &biggest);
    char body[120];
    snprintf(
        body,
        sizeof(body),
        "Free: %u KB  Block: %u KB\nNeeds: %u KB (+%u spare)\nBiggest piece: %u KB\n%s",
        PT_KB(memmgr_get_free_heap()),
        PT_KB(memmgr_heap_get_max_free_block()),
        PT_KB(total),
        PT_KB(PT_MEM_MARGIN),
        PT_KB(biggest),
        pt_memory_ok(g) ? "It fits" : "It does NOT fit");
    pt_show_info(app, StepGenMem, "Memory", body);
}

static void pt_show_mem_error(PokemonTypesApp* app, const GenerationInfo* g) {
    size_t biggest;
    size_t total = pt_memory_needed(g, &biggest);
    char body[120];
    snprintf(
        body,
        sizeof(body),
        "Needs %u KB + %u spare\nFree: %u KB  Block: %u KB\nBack = choose again",
        PT_KB(total),
        PT_KB(PT_MEM_MARGIN),
        PT_KB(memmgr_get_free_heap()),
        PT_KB(memmgr_heap_get_max_free_block()));
    pt_show_info(app, StepMemError, "Not enough memory", body);
}

// From the main menu: what the loaded generation actually cost.
static void pt_show_main_mem(PokemonTypesApp* app) {
    char body[140];
    size_t before = app->mem_free_before;
    size_t after = app->mem_free_after;
    snprintf(
        body,
        sizeof(body),
        "Data uses: %u KB\nFree: %u KB  Block: %u KB\nLowest free: %u KB\nTotal heap: %u KB",
        PT_KB(before > after ? before - after : 0),
        PT_KB(memmgr_get_free_heap()),
        PT_KB(memmgr_heap_get_max_free_block()),
        PT_KB(memmgr_get_minimum_free_heap()),
        PT_KB(memmgr_get_total_heap()));
    pt_show_info(app, StepMainMem, "Memory Info", body);
}

// --- Session file (remembers the last generation) --------------------------

#define PT_SESSION_FILE POKEDEX_DATA_DIR "/session.txt"

// Returns the saved generation index, or -1 if there is none (first launch).
static int pt_session_load(void) {
    int result = -1;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, PT_SESSION_FILE, FSAM_READ, FSOM_OPEN_EXISTING)) {
        char buf[4] = {0};
        size_t n = stream_read(stream, (uint8_t*)buf, sizeof(buf) - 1);
        if(n > 0 && buf[0] >= '0' && buf[0] <= '9') {
            int v = atoi(buf);
            if(v >= 0 && v < (int)GENERATION_COUNT) result = v;
        }
        file_stream_close(stream);
    }
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
    return result;
}

static void pt_session_save(int gen_index) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, POKEDEX_DATA_DIR);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, PT_SESSION_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[12];
        int n = snprintf(buf, sizeof(buf), "%d", gen_index);
        stream_write(stream, (const uint8_t*)buf, (size_t)n);
        file_stream_close(stream);
    }
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

// --- Title page ----------------------------------------------------------------

typedef struct {
    bool has_continue;
    uint8_t selected; // 0 = first item shown
} TitleModel;

// 5x7 block letters: one byte per row, bit 4 = leftmost pixel.
typedef struct {
    char ch;
    uint8_t rows[7];
} PtGlyph;

static const PtGlyph pt_glyphs[] = {
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
};

// Draws text as scaled pixel blocks, each row shifted right a little more
// towards the top so the lettering leans (italic). Spaces are blank cells.
static void pt_draw_block_text(Canvas* canvas, int x, int y, const char* text, int scale, bool slant) {
    int cell = 6 * scale; // 5 pixel columns + 1 gap
    for(int i = 0; text[i]; i++) {
        const PtGlyph* g = NULL;
        for(size_t k = 0; k < sizeof(pt_glyphs) / sizeof(pt_glyphs[0]); k++) {
            if(pt_glyphs[k].ch == text[i]) g = &pt_glyphs[k];
        }
        if(!g) continue;
        for(int r = 0; r < 7; r++) {
            int shift = slant ? (6 - r) / 2 : 0;
            for(int c = 0; c < 5; c++) {
                if(g->rows[r] & (0x10 >> c)) {
                    canvas_draw_box(
                        canvas, x + i * cell + c * scale + shift, y + r * scale, scale, scale);
                }
            }
        }
    }
}

static void pt_draw_pokeball(Canvas* canvas, int cx, int cy) {
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_disc(canvas, cx, cy, 6); // white ball
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, cx - 6, cy + 1, 13, 7); // bottom half hollow
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_circle(canvas, cx, cy, 6); // outline
    canvas_draw_line(canvas, cx - 6, cy, cx + 6, cy); // belt
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_disc(canvas, cx, cy, 2); // button
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_circle(canvas, cx, cy, 2);
}

static void pt_title_draw_callback(Canvas* canvas, void* model_ptr) {
    const TitleModel* model = model_ptr;
    canvas_clear(canvas);

    // Black banner with the lettering in white.
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_box(canvas, 0, 0, 128, 35);
    canvas_set_color(canvas, ColorWhite);
    pt_draw_block_text(canvas, 23, 4, "POKEMON", 2, true);
    pt_draw_block_text(canvas, 28, 22, "TYPE CHECKER", 1, true);
    canvas_draw_line(canvas, 8, 32, 119, 32);
    pt_draw_pokeball(canvas, 11, 17);
    pt_draw_pokeball(canvas, 117, 17);

    // Menu.
    const char* items[2];
    int count = 0;
    if(model->has_continue) items[count++] = "Continue Session";
    items[count++] = "New Session";
    canvas_set_font(canvas, FontPrimary);
    for(int i = 0; i < count; i++) {
        int y = (count == 1) ? 44 : 38 + i * 13;
        bool sel = (model->selected == i);
        canvas_set_color(canvas, ColorBlack);
        if(sel) {
            canvas_draw_rbox(canvas, 12, y, 104, 12, 3);
            canvas_set_color(canvas, ColorWhite);
        }
        canvas_draw_str_aligned(canvas, 64, y + 6, AlignCenter, AlignCenter, items[i]);
        canvas_set_color(canvas, ColorBlack);
    }
}

static bool pt_title_input_callback(InputEvent* event, void* context) {
    PokemonTypesApp* app = context;
    if(event->type != InputTypeShort) return false;

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        TitleModel* model = view_get_model(app->title_view);
        if(model->has_continue) model->selected = model->selected ? 0 : 1;
        view_commit_model(app->title_view, true);
        return true;
    }
    if(event->key == InputKeyOk) {
        TitleModel* model = view_get_model(app->title_view);
        bool is_continue = model->has_continue && model->selected == 0;
        view_commit_model(app->title_view, false);
        if(is_continue) {
            app->gen_choice = app->saved_gen;
            pt_confirm_generation(app); // skips the picker and the games list
        } else {
            if(app->saved_gen >= 0) app->gen_choice = app->saved_gen;
            pt_populate_gen_select(app);
        }
        return true;
    }
    return false; // Back reaches the navigation callback (exit)
}

static void pt_show_title(PokemonTypesApp* app) {
    pt_unload_generation(app);
    app->saved_gen = pt_session_load();
    TitleModel* model = view_get_model(app->title_view);
    model->has_continue = app->saved_gen >= 0;
    model->selected = 0;
    view_commit_model(app->title_view, true);

    app->step = StepTitle;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewTitle);
}

// Loads the generation chosen in app->gen_choice (after the memory check) and
// opens the main menu. Used by the Confirm button and by "Continue Session".
static void pt_confirm_generation(PokemonTypesApp* app) {
    const GenerationInfo* g = &generations[app->gen_choice];
    if(!pt_memory_ok(g)) {
        pt_show_mem_error(app, g);
        return;
    }
    // Confirmed: this generation's chart, types and Pokedex file are now "the" data.
    app->gen = g;
    pt_session_save(app->gen_choice);
    app->mem_free_before = memmgr_get_free_heap();
    pokedex_load(app, app->gen);
    app->mem_free_after = memmgr_get_free_heap();
    pt_populate_main_menu(app);
}

static void pt_gen_confirm_button_callback(GuiButtonType result, InputType type, void* context) {
    PokemonTypesApp* app = context;
    if(type != InputTypeShort) return;

    if(result == GuiButtonTypeLeft) {
        pt_populate_gen_select(app);
    } else if(result == GuiButtonTypeCenter) {
        pt_show_gen_mem(app);
    } else if(result == GuiButtonTypeRight) {
        pt_confirm_generation(app);
    }
}

// The games list is a scrolling text box (Up/Down), so a generation with more
// games than fit on screen (Generation 3 has five) can still show them all.
static void pt_show_gen_confirm(PokemonTypesApp* app) {
    const GenerationInfo* g = &generations[app->gen_choice];

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 2, AlignCenter, AlignTop, FontPrimary, g->dialog_title);
    widget_add_text_scroll_element(app->widget, 0, 14, 128, 36, g->games);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Back", pt_gen_confirm_button_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Mem", pt_gen_confirm_button_callback, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeRight, "Confirm", pt_gen_confirm_button_callback, app);

    app->step = StepGenConfirm;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewResult);
}

// --- Ability rules for the type checker -------------------------------------
//
// How the abilities that change a matchup are applied. Rules are looked up by
// the ability's NAME, so the Pokedex data files, the forms data and this table
// only have to agree on spelling. 'from_gen' is the first generation in which
// the rule applies (Lightning Rod only absorbs Electric moves from Generation 5
// on). 'breakable' says whether an attacker with Mold Breaker, Teravolt or
// Turboblaze skips the ability.

typedef enum {
    PtRuleImmune, // type_a / type_b do no damage
    PtRuleHalf, // type_a / type_b damage is halved
    PtRuleDouble, // type_a / type_b damage is doubled
    PtRuleReduceSE, // super effective hits do 25% less
    PtRuleOnlySE, // Wonder Guard: only super effective hits do damage
    PtRuleNoFlyWeak, // Delta Stream: Flying loses its weaknesses
    PtRuleTeraShell, // Tera Shell: every damaging hit is "not very effective"
    PtRuleDrySkin, // Dry Skin: Water does no damage, Fire does x1.25
} PtRuleKind;

// Type positions in type_names[].
enum {
    PT_TY_NORMAL,
    PT_TY_FIRE,
    PT_TY_WATER,
    PT_TY_ELECTRIC,
    PT_TY_GRASS,
    PT_TY_ICE,
    PT_TY_FIGHTING,
    PT_TY_POISON,
    PT_TY_GROUND,
    PT_TY_FLYING,
    PT_TY_PSYCHIC,
    PT_TY_BUG,
    PT_TY_ROCK,
    PT_TY_GHOST,
};

typedef struct {
    const char* name;
    uint8_t kind;
    uint8_t type_a;
    uint8_t type_b;
    uint8_t from_gen;
    uint8_t breakable;
    const char* note; // extra remark shown on the result screen (NULL = none)
} PtMatchupRule;

static const PtMatchupRule matchup_rules[] = {
    {"Levitate", PtRuleImmune, PT_TY_GROUND, PT_NO_TYPE, 3, 1, NULL},
    {"Eelevate", PtRuleImmune, PT_TY_GROUND, PT_NO_TYPE, 9, 1, NULL},
    {"Earth Eater", PtRuleImmune, PT_TY_GROUND, PT_NO_TYPE, 9, 1, NULL},
    {"Volt Absorb", PtRuleImmune, PT_TY_ELECTRIC, PT_NO_TYPE, 3, 1, NULL},
    {"Motor Drive", PtRuleImmune, PT_TY_ELECTRIC, PT_NO_TYPE, 4, 1, NULL},
    {"Lightning Rod", PtRuleImmune, PT_TY_ELECTRIC, PT_NO_TYPE, 5, 1, NULL},
    {"Water Absorb", PtRuleImmune, PT_TY_WATER, PT_NO_TYPE, 3, 1, NULL},
    {"Storm Drain", PtRuleImmune, PT_TY_WATER, PT_NO_TYPE, 5, 1, NULL},
    {"Dry Skin", PtRuleDrySkin, PT_TY_WATER, PT_NO_TYPE, 4, 1, NULL},
    {"Flash Fire", PtRuleImmune, PT_TY_FIRE, PT_NO_TYPE, 3, 1, NULL},
    {"Well-Baked Body", PtRuleImmune, PT_TY_FIRE, PT_NO_TYPE, 9, 1, NULL},
    {"Sap Sipper", PtRuleImmune, PT_TY_GRASS, PT_NO_TYPE, 5, 1, NULL},
    {"Thick Fat", PtRuleHalf, PT_TY_FIRE, PT_TY_ICE, 3, 1, NULL},
    {"Heatproof", PtRuleHalf, PT_TY_FIRE, PT_NO_TYPE, 4, 1, NULL},
    {"Water Bubble", PtRuleHalf, PT_TY_FIRE, PT_NO_TYPE, 7, 1, NULL},
    {"Purifying Salt", PtRuleHalf, PT_TY_GHOST, PT_NO_TYPE, 9, 1, NULL},
    {"Fluffy", PtRuleDouble, PT_TY_FIRE, PT_NO_TYPE, 7, 1, "1/2 damage if contact move"},
    {"Filter", PtRuleReduceSE, PT_NO_TYPE, PT_NO_TYPE, 4, 1, NULL},
    {"Solid Rock", PtRuleReduceSE, PT_NO_TYPE, PT_NO_TYPE, 4, 1, NULL},
    {"Prism Armor", PtRuleReduceSE, PT_NO_TYPE, PT_NO_TYPE, 7, 0, NULL},
    {"Wonder Guard", PtRuleOnlySE, PT_NO_TYPE, PT_NO_TYPE, 3, 1, NULL},
    {"Delta Stream", PtRuleNoFlyWeak, PT_NO_TYPE, PT_NO_TYPE, 6, 0, NULL},
    {"Tera Shell", PtRuleTeraShell, PT_NO_TYPE, PT_NO_TYPE, 9, 1, "only w/ full HP"},
};
#define MATCHUP_RULE_COUNT (sizeof(matchup_rules) / sizeof(matchup_rules[0]))

static const PtMatchupRule* pt_find_rule(const char* ability_name) {
    for(size_t i = 0; i < MATCHUP_RULE_COUNT; i++) {
        if(strcmp(matchup_rules[i].name, ability_name) == 0) return &matchup_rules[i];
    }
    return NULL;
}

// Damage multiplier of an attack type against a defender, in sixteenths
// (16 = x1, 32 = x2, 8 = x1/2, 0 = no effect). 'rule' is the defender's
// ability rule, or NULL for none. The type chart stores quarters (4 = x1), so
// finer steps (x1/8, x3, x6, x8...) show up once abilities are involved.
static int pt_damage16(
    const GenerationInfo* gen,
    int generation,
    int atk,
    int t1,
    int t2,
    const PtMatchupRule* rule) {
    if(rule && generation < rule->from_gen) rule = NULL;

    int m1 = gen->chart[atk][t1];
    int m2 = t2 >= 0 ? gen->chart[atk][t2] : 4;
    if(rule && rule->kind == PtRuleNoFlyWeak) {
        if(t1 == PT_TY_FLYING && m1 > 4) m1 = 4;
        if(t2 == PT_TY_FLYING && m2 > 4) m2 = 4;
    }
    int r = m1 * m2; // 16 = x1 already

    if(!rule) return r;
    bool hits_type = (atk == rule->type_a) || (atk == rule->type_b);
    switch(rule->kind) {
    case PtRuleImmune:
        if(hits_type) r = 0;
        break;
    case PtRuleHalf:
        if(hits_type) r /= 2;
        break;
    case PtRuleDouble:
        if(hits_type) r *= 2;
        break;
    case PtRuleReduceSE:
        if(r > 16) r = r * 3 / 4;
        break;
    case PtRuleOnlySE:
        if(r <= 16) r = 0;
        break;
    case PtRuleTeraShell:
        if(r > 0) r = 8;
        break;
    case PtRuleDrySkin:
        if(atk == PT_TY_WATER) r = 0;
        if(atk == PT_TY_FIRE) r = r * 5 / 4;
        break;
    default:
        break;
    }
    return r;
}

// --- Type effectiveness screens --------------------------------------------

static int pt_gen_number(const PokemonTypesApp* app) {
    return (int)(app->gen - generations) + 1;
}

// "x1/2", "x2"... for a multiplier in sixteenths (see pt_damage16).
static void pt_mult_text(int r16, char* out, size_t out_size) {
    const char* t;
    switch(r16) {
    case 0:
        t = "x0";
        break;
    case 2:
        t = "x1/8";
        break;
    case 4:
        t = "x1/4";
        break;
    case 6:
        t = "x3/8";
        break;
    case 8:
        t = "x1/2";
        break;
    case 5:
        t = "x5/16";
        break;
    case 10:
        t = "x5/8";
        break;
    case 12:
        t = "x3/4";
        break;
    case 16:
        t = "x1";
        break;
    case 20:
        t = "x1.25";
        break;
    case 24:
        t = "x1.5";
        break;
    case 32:
        t = "x2";
        break;
    case 40:
        t = "x2.5";
        break;
    case 48:
        t = "x3";
        break;
    case 64:
        t = "x4";
        break;
    case 80:
        t = "x5";
        break;
    case 96:
        t = "x6";
        break;
    case 128:
        t = "x8";
        break;
    case 160:
        t = "x10";
        break;
    default:
        snprintf(out, out_size, "x%d/16", r16);
        return;
    }
    snprintf(out, out_size, "%s", t);
}

static const char* pt_class_text(int r16) {
    if(r16 == 0) return "No Effect";
    if(r16 < 16) return "Not Very Effective";
    if(r16 == 16) return "Normal Damage";
    return "Super Effective";
}

// The result screen: the plain type matchup, and, when the defender came from
// the Pokedex, what its abilities do to that matchup.
static void pt_show_type_result(PokemonTypesApp* app) {
    const GenerationInfo* gen = app->gen;
    int generation = pt_gen_number(app);
    int atk = app->attack_type;
    int t1 = app->defense_type1;
    int t2 = app->defense_type2;
    const PtDefender* d = &app->defender;
    int base = pt_damage16(gen, generation, atk, t1, t2, NULL);

    // Result with each of the defender's abilities.
    int ab_result[3];
    int changed = 0;
    bool same = true;
    int first_changed = -1;
    if(app->defender_is_pokemon) {
        for(int i = 0; i < d->ab_count; i++) {
            const PtMatchupRule* rule = pt_find_rule(d->ab[i]);
            if(rule && app->ignore_abilities && rule->breakable) rule = NULL;
            ab_result[i] = pt_damage16(gen, generation, atk, t1, t2, rule);
            if(ab_result[i] != base) {
                changed++;
                if(first_changed < 0) first_changed = i;
                else if(ab_result[i] != ab_result[first_changed]) same = false;
            }
        }
    }
    // Every ability gives the same different result: that is "the" result.
    bool all_abilities = changed > 0 && changed == d->ab_count && same;
    int headline = all_abilities ? ab_result[first_changed] : base;

    char mult[16];
    FuriString* body = furi_string_alloc();
    if(all_abilities) {
        pt_mult_text(headline, mult, sizeof(mult));
        furi_string_cat_printf(body, "%s%sdue to ", headline ? mult : "", headline ? " " : "");
        bool first = true;
        for(int i = 0; i < d->ab_count; i++) {
            furi_string_cat_printf(body, "%s%s", first ? "" : " or ", d->ab[i]);
            first = false;
        }
        furi_string_cat_printf(body, "\n");
    } else if(base != 0) {
        pt_mult_text(base, mult, sizeof(mult));
        furi_string_cat_printf(body, "%s\n", mult);
    }
    if(!all_abilities) {
        for(int i = 0; i < d->ab_count; i++) {
            if(ab_result[i] == base) continue;
            pt_mult_text(ab_result[i], mult, sizeof(mult));
            furi_string_cat_printf(
                body,
                "%s%s: %s\n",
                d->ab[i],
                d->ab_hidden[i] ? " (HA)" : "",
                ab_result[i] ? mult : "No Effect");
        }
    }
    if(app->defender_is_pokemon) {
        for(int i = 0; i < d->ab_count; i++) {
            const PtMatchupRule* rule = pt_find_rule(d->ab[i]);
            if(!rule || !rule->note || generation < rule->from_gen) continue;
            if(app->ignore_abilities && rule->breakable) continue;
            furi_string_cat_printf(body, "%s: %s\n", d->ab[i], rule->note);
        }
        if(d->label[0]) furi_string_cat_printf(body, "Form: %s\n", d->label);
        furi_string_cat_printf(
            body,
            "Types: %s%s%s\n",
            type_names[t1],
            t2 >= 0 ? "/" : "",
            t2 >= 0 ? type_names[t2] : "");
        if(app->ignore_abilities && generation >= 3) {
            furi_string_cat_printf(body, "(Attacker ignores abilities)\n");
        }
    }

    FuriString* header = furi_string_alloc();
    if(app->defender_is_pokemon) {
        furi_string_printf(header, "%s vs %s", type_names[atk], d->name);
    } else if(t2 >= 0) {
        furi_string_printf(header, "%s vs %s/%s", type_names[atk], type_names[t1], type_names[t2]);
    } else {
        furi_string_printf(header, "%s vs %s", type_names[atk], type_names[t1]);
    }

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 6, AlignCenter, AlignCenter, FontSecondary, furi_string_get_cstr(header));
    widget_add_string_element(
        app->widget, 64, 19, AlignCenter, AlignCenter, FontPrimary, pt_class_text(headline));
    if(furi_string_size(body) > 0) {
        widget_add_text_scroll_element(app->widget, 0, 26, 128, 28, furi_string_get_cstr(body));
    }
    widget_add_string_element(
        app->widget, 64, 59, AlignCenter, AlignCenter, FontSecondary, "Back = new search");

    furi_string_free(header);
    furi_string_free(body);

    app->step = StepTypeResult;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewResult);
}

// "Select Defender PKMN" leads here: find the Pokemon by name, or by browsing.
static void pt_populate_defender_source(PokemonTypesApp* app) {
    app->pick_mode = true;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Select Defender PKMN");
    submenu_add_item(app->submenu, "Search", 0, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "Browse", 1, pt_submenu_callback, app);
    app->step = StepDefenderSource;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// The screen between choosing the attack type and choosing the defender.
static void pt_populate_defense_options(PokemonTypesApp* app) {
    app->pick_mode = false;
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Defense Options");
    submenu_add_item(app->submenu, "Select Type Combo", 0, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "Select Defender PKMN", 1, pt_submenu_callback, app);
    if(pt_gen_number(app) >= 3) { // abilities exist from Generation 3
        submenu_add_item(
            app->submenu,
            app->ignore_abilities ? "Ignore abilities: On" : "Ignore abilities: Off",
            2,
            pt_submenu_callback,
            app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)app->options_choice);
    app->step = StepDefenseOptions;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// Adds one submenu item per type that exists in the confirmed generation (so in
// Generation 1 there is no Dark, Steel or Fairy). Each item's id is the type's
// index in type_names, which is what the click handler receives. Pass the
// index of a type to leave out of the list, or -1 to include them all.
static void pt_add_type_items(PokemonTypesApp* app, int skip_type) {
    for(int i = 0; i < app->gen->type_count; i++) {
        if(i == skip_type) continue;
        submenu_add_item(app->submenu, type_names[i], (uint32_t)i, pt_submenu_callback, app);
    }
}

static void pt_populate_type_attack(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Select Attack Type");
    pt_add_type_items(app, -1);
    app->step = StepTypeAttack;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

static void pt_populate_type_defense1(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Select Defense Type 1");
    pt_add_type_items(app, -1);
    app->step = StepTypeDefense1;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

static void pt_populate_type_defense2(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Add 2nd Type?");
    // "None" gets the id TYPE_COUNT (18), which is never a real type index.
    submenu_add_item(app->submenu, "None / Single Type", TYPE_COUNT, pt_submenu_callback, app);
    // A Pokemon can't have the same type twice, so the first type isn't offered again.
    pt_add_type_items(app, app->defense_type1);
    app->step = StepTypeDefense2;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// --- Pokedex screens ---------------------------------------------------

static void pt_show_pokedex_error(PokemonTypesApp* app) {
    FuriString* msg = furi_string_alloc();
    furi_string_printf(
        msg, "Copy %s\nto SD Card folder:\napps_data/pokemon_types", app->gen->csv_name);

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 12, AlignCenter, AlignCenter, FontPrimary, "File not found");
    widget_add_string_multiline_element(
        app->widget, 64, 40, AlignCenter, AlignCenter, FontSecondary, furi_string_get_cstr(msg));

    furi_string_free(msg);

    app->step = StepPokedexError;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewResult);
}

static void pt_search_result_callback(void* context) {
    PokemonTypesApp* app = context;
    app->browse_mode = false;
    app->browse_kind = 0;
    snprintf(app->list_title, sizeof(app->list_title), "Results");
    app->selected_position = 0;
    pokedex_filter(app);
    pt_populate_pokedex_results(app);
}

static void pt_show_pokedex_search(PokemonTypesApp* app) {
    text_input_reset(app->text_input);
    text_input_set_header_text(
        app->text_input, app->pick_mode ? "Search Defender" : "Search Pokemon");
    text_input_set_result_callback(
        app->text_input,
        pt_search_result_callback,
        app,
        app->search_buffer,
        sizeof(app->search_buffer),
        true);
    app->step = StepPokedexSearch;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewTextInput);
}

static void pt_populate_pokedex_results(PokemonTypesApp* app) {
    submenu_reset(app->submenu);

    // Long lists are shown one page at a time (see PT_PAGE).
    int page = 0;
    if(app->selected_position >= 0 && app->selected_position < app->filtered_count) {
        page = app->selected_position / PT_PAGE;
    }
    int first = page * PT_PAGE;
    int last = first + PT_PAGE;
    if(last > app->filtered_count) last = app->filtered_count;

    const char* title = app->pick_mode ? "Defender" :
                        app->list_title[0] ? app->list_title :
                                             (app->browse_mode ? "Pokedex" : "Results");
    if(app->filtered_count <= PT_PAGE) {
        snprintf(
            app->results_header, sizeof(app->results_header), "%s (%d)", title, app->filtered_count);
    } else {
        snprintf(
            app->results_header,
            sizeof(app->results_header),
            "%s %d-%d/%d",
            title,
            first + 1,
            last,
            app->filtered_count);
    }
    submenu_set_header(app->submenu, app->results_header);

    if(first > 0) {
        submenu_add_item(
            app->submenu, "< Previous page", PT_ID_PREV_PAGE, pt_submenu_callback, app);
    }
    for(int i = first; i < last; i++) {
        int pokedex_idx = PT_FI_ENTRY(app->filtered_indices[i]);
        int form = PT_FI_FORM(app->filtered_indices[i]);
        // The submenu keeps its own copy of the text, so a temporary buffer will do.
        char label[48];
        PtDefender f;
        if(form >= 0 && pt_form_get(app, &app->pokedex[pokedex_idx], form, &f)) {
            snprintf(label, sizeof(label), "%s (%s)", app->pokedex[pokedex_idx].name, f.label);
        } else {
            snprintf(
                label,
                sizeof(label),
                "#%03d %s",
                app->pokedex[pokedex_idx].number,
                app->pokedex[pokedex_idx].name);
        }
        submenu_add_item(app->submenu, label, (uint32_t)i, pt_submenu_callback, app);
    }
    if(last < app->filtered_count) {
        submenu_add_item(app->submenu, "Next page >", PT_ID_NEXT_PAGE, pt_submenu_callback, app);
    }

    // Put the cursor back on the entry we were just viewing, so pressing Back
    // from a detail screen doesn't throw you back to the top of the list.
    if(app->filtered_count > 0) {
        submenu_set_selected_item(app->submenu, (uint32_t)app->selected_position);
    }

    app->step = StepPokedexResults;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// The list of groups ("#001-#151  Kanto", ...) that Browse starts from.
static void pt_populate_pokedex_groups(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, app->pick_mode ? "Pick Defender" : "Pokedex Browse");
    for(int i = 0; i < app->gen->group_count; i++) {
        submenu_add_item(app->submenu, app->gen->groups[i].label, (uint32_t)i, pt_submenu_callback, app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)app->group_choice);

    app->step = StepPokedexGroups;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// Shows the Pokemon of one group as a list.
static void pt_open_pokedex_group(PokemonTypesApp* app, int group_index) {
    app->group_choice = group_index;
    app->browse_mode = true;
    app->browse_kind = 0;
    app->list_title[0] = '\0';
    app->search_buffer[0] = '\0';
    app->selected_position = 0;
    pokedex_filter_group(app, &app->gen->groups[group_index]);
    pt_populate_pokedex_results(app);
}

// "Pokedex Browse": pick a group first, unless the generation has just one.
static void pt_start_pokedex_browse(PokemonTypesApp* app) {
    if(app->gen->group_count > 1) {
        pt_populate_pokedex_groups(app);
    } else {
        pt_open_pokedex_group(app, 0);
    }
}

// "Pokedex Browse" starts here: by number (the groups), by type or by ability.
static void pt_populate_browse_by(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Pokedex Browse");
    submenu_add_item(app->submenu, "By Number", 0, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "By Type", 1, pt_submenu_callback, app);
    if((app->gen - generations) >= 2) { // abilities exist from Generation 3
        submenu_add_item(app->submenu, "By Ability", 2, pt_submenu_callback, app);
    }
    app->step = StepBrowseBy;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

static void pt_populate_browse_types(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Browse by Type");
    pt_add_type_items(app, -1);
    app->step = StepBrowseType;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// Which abilities does the loaded Pokedex use? One bit per entry of abilities[].
#define PT_ABILITY_BYTES ((ABILITY_COUNT + 7) / 8)

static void pt_abilities_in_use(const PokemonTypesApp* app, uint8_t* bits) {
    memset(bits, 0, PT_ABILITY_BYTES);
    char token[PT_TOKEN_MAX];
    for(int i = 0; i < app->pokedex_count; i++) {
        for(int k = 0; k < 3; k++) {
            uint16_t a = app->pokedex[i].ability[k];
            if(a != 0) bits[a / 8] |= (uint8_t)(1 << (a % 8));
        }
        // Abilities that only a form has.
        const char* cursor = pt_entry_formdata(app, &app->pokedex[i]);
        while(pt_next_part_sep(&cursor, token, sizeof(token), '|')) {
            PtDefender d;
            pt_parse_form(token, &d);
            for(int k = 0; k < d.ab_count; k++) {
                uint16_t a = pt_ability_index(d.ab[k]);
                if(a != 0) bits[a / 8] |= (uint8_t)(1 << (a % 8));
            }
        }
    }
}

static char pt_ability_letter(uint16_t index) {
    char c = abilities[index].name[0];
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

// An ability list would be too long to scroll through, so first pick a letter.
static void pt_populate_browse_letters(PokemonTypesApp* app) {
    uint8_t bits[PT_ABILITY_BYTES];
    pt_abilities_in_use(app, bits);
    bool letter_used[26] = {false};
    for(size_t a = 1; a < ABILITY_COUNT; a++) {
        if(!(bits[a / 8] & (1 << (a % 8)))) continue;
        char c = pt_ability_letter((uint16_t)a);
        if(c >= 'A' && c <= 'Z') letter_used[c - 'A'] = true;
    }
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Ability starts with");
    for(int i = 0; i < 26; i++) {
        if(!letter_used[i]) continue;
        char label[2] = {(char)('A' + i), '\0'};
        submenu_add_item(app->submenu, label, (uint32_t)('A' + i), pt_submenu_callback, app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)app->browse_letter);
    app->step = StepBrowseLetter;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

static void pt_populate_browse_abilities(PokemonTypesApp* app) {
    uint8_t bits[PT_ABILITY_BYTES];
    pt_abilities_in_use(app, bits);
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Pick an Ability");
    for(size_t a = 1; a < ABILITY_COUNT; a++) {
        if(!(bits[a / 8] & (1 << (a % 8)))) continue;
        if(pt_ability_letter((uint16_t)a) != app->browse_letter) continue;
        submenu_add_item(app->submenu, abilities[a].name, (uint32_t)a, pt_submenu_callback, app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)app->browse_arg);
    app->step = StepBrowseAbility;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// The list of Pokemon with the chosen type (kind 1) or ability (kind 2).
static void pt_open_browse_list(PokemonTypesApp* app, int kind, int arg) {
    app->browse_kind = kind;
    app->browse_arg = arg;
    app->browse_mode = true;
    app->selected_position = 0;
    if(kind == 1) {
        snprintf(app->list_title, sizeof(app->list_title), "%s", type_names[arg]);
        pokedex_filter_attr(app, 1, arg);
    } else {
        snprintf(app->list_title, sizeof(app->list_title), "%s", abilities[arg].name);
        pokedex_filter_attr(app, 2, arg);
    }
    pt_populate_pokedex_results(app);
}

// --- Pokedex detail screen (custom View) -------------------------------
//
// The Widget module can't listen for the arrow keys, so this screen is a plain View
// that we draw ourselves with canvas calls and give our own input handler.
// Coordinates: the Flipper screen is 128 x 64 pixels, (0,0) = top-left.

// Draws a small filled triangle, 3 pixels wide and 5 tall, vertically centred
// on cy, with its left edge at x. It is built from three vertical lines of
// length 1, 3 and 5; the tip is the short one.
static void pt_draw_arrow(Canvas* canvas, int x, int cy, bool pointing_left) {
    for(int i = 0; i < 3; i++) {
        int half = pointing_left ? i : 2 - i; // pointing left: tip (shortest line) is at x
        canvas_draw_line(canvas, x + i, cy - half, x + i, cy + half);
    }
}

// --- Text rows -------------------------------------------------------------
//
// Under the name and types, the screen shows a scrollable body made of text
// "rows" (evolutions first, then abilities). The same code that draws the rows
// is also run with no canvas, just to count them, so the input handler knows
// how far down the body can be scrolled.

#define PT_BODY_Y 24 // top of the first body row, in pixels
#define PT_ROW_H 9 // pixels per row (the secondary font's recommended minimum line spacing)
#define PT_BODY_ROWS 4 // rows that fit on screen below the header
#define PT_WRAP_PX 116 // widest a row may be; keeps text clear of the scroll bar and arrows

// How wide each printable character (' ' to '~') is in the Flipper's secondary
// font, in pixels. canvas_string_width() would give this, but it needs a Canvas
// and the input handler (which has to know how many rows there are) doesn't
// have one. These numbers were read from the firmware's own font data.
static const uint8_t pt_font_width[95] = {
    2, 2, 4, 6, 6, 8, 7, 2, 4, 4, 6, 6, 3, 6, 2, 8, 6, 3, 6,
    6, 6, 6, 6, 6, 6, 6, 2, 3, 4, 6, 4, 6, 9, 6, 6, 6, 6, 6,
    6, 6, 6, 2, 6, 6, 6, 8, 6, 6, 6, 6, 6, 6, 6, 6, 6, 8, 6,
    6, 6, 4, 8, 4, 6, 6, 4, 5, 5, 5, 5, 5, 3, 5, 5, 2, 3, 5,
    2, 8, 5, 5, 5, 5, 4, 4, 4, 5, 5, 8, 5, 5, 5, 4, 2, 4, 7,
};

static int pt_char_width(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= ' ' && u <= '~') ? pt_font_width[u - ' '] : 0;
}

typedef struct {
    Canvas* canvas; // NULL = only count rows, don't draw
    int scroll; // first row that is visible
    int row; // rows emitted so far
} PtRows;

// Emits one row. It is drawn only if it falls inside the visible window.
static void pt_row(PtRows* r, const char* text) {
    if(r->canvas && r->row >= r->scroll && r->row < r->scroll + PT_BODY_ROWS) {
        int y = PT_BODY_Y + (r->row - r->scroll) * PT_ROW_H;
        canvas_draw_str_aligned(r->canvas, 2, y, AlignLeft, AlignTop, text);
    }
    r->row++;
}

// Emits text as one or more rows, breaking between words so that no row is
// wider than PT_WRAP_PX pixels. The first row starts 'first_indent' spaces in
// and continuation rows are indented by two. A single word that is too wide
// for a row is split in the middle.
static void pt_row_wrapped(PtRows* r, int first_indent, const char* text) {
    char line[64];
    int n = 0; // characters currently in line[]
    int width = 0; // their total width in pixels
    int indent = first_indent; // leading spaces of the row being built (2 after the first row)
    bool has_word = false; // does line[] hold a word yet (beyond the indent)?
    const int space_w = pt_char_width(' ');
    const char* p = text;

    while(*p != '\0') {
        while(*p == ' ') {
            p++;
        }
        if(*p == '\0') break;

        const char* start = p;
        int word_w = 0;
        while(*p != '\0' && *p != ' ') {
            word_w += pt_char_width(*p);
            p++;
        }
        int word_len = (int)(p - start);

        // Doesn't fit after what is already on this row: finish the row.
        if(has_word && width + space_w + word_w > PT_WRAP_PX) {
            line[n] = '\0';
            pt_row(r, line);
            indent = 2;
            has_word = false;
        }

        if(has_word) {
            line[n++] = ' '; // space between words
            width += space_w;
        } else {
            n = 0;
            width = 0;
            for(int i = 0; i < indent; i++) {
                line[n++] = ' ';
                width += space_w;
            }
        }

        for(int i = 0; i < word_len; i++) {
            int cw = pt_char_width(start[i]);
            if(width + cw > PT_WRAP_PX || n >= (int)sizeof(line) - 1) { // word too wide: break it
                line[n] = '\0';
                pt_row(r, line);
                n = 0;
                width = 0;
                for(int k = 0; k < 2; k++) {
                    line[n++] = ' ';
                    width += space_w;
                }
            }
            line[n++] = start[i];
            width += cw;
        }
        has_word = true;
    }

    if(has_word) {
        line[n] = '\0';
        pt_row(r, line);
    }
}

// Lists in the CSV are written "A/B/C".
static int pt_count_parts(const char* list) {
    int n = 1;
    for(const char* p = list; *p != '\0'; p++) {
        if(*p == '/') n++;
    }
    return n;
}

// Evolution rows. EvolvesTo is "A" or "A/B/C"; EvoMethod is either one method
// for all of them, or one per target ("m1/m2/m3", same order).
//   one target          ->  "-> Ivysaur"  then  "(Level 16)"
//   one method for all  ->  one target per row, then "(method)"
//   one method each     ->  "Name: method" per target
static void pt_rows_evolution(PtRows* r, const char* to_all, const char* how_all) {
    if(strcmp(to_all, "-") == 0) {
        pt_row(r, "Does not evolve");
        return;
    }

    int n_to = pt_count_parts(to_all);
    bool paired = (n_to > 1) && (pt_count_parts(how_all) == n_to);
    char target[24];
    char method[96];
    char buf[128];

    const char* to = to_all;
    const char* how = how_all;
    bool first = true;
    while(pt_next_part(&to, target, sizeof(target))) {
        if(paired) {
            pt_next_part(&how, method, sizeof(method));
            snprintf(buf, sizeof(buf), "%s: %s", target, method);
        } else {
            snprintf(buf, sizeof(buf), "%s%s", first ? "-> " : "", target);
        }
        pt_row_wrapped(r, (paired || first) ? 0 : 3, buf);
        first = false;
    }

    if(!paired) {
        snprintf(buf, sizeof(buf), "(%s)", how_all);
        pt_row_wrapped(r, 0, buf);
    }
}

// Ability rows: "Abilities:" then one entry per ability, with its effect after
// the name when it has one. The Hidden Ability is marked "(HA)".
static void pt_rows_abilities(PtRows* r, const PtDefender* d, int generation) {
    if(d->ab_unknown) {
        pt_row(r, "Abilities: not known");
        return;
    }
    if(d->ab_count == 0) return;

    char buf[96];
    pt_row(r, "Abilities:");
    for(int i = 0; i < d->ab_count; i++) {
        uint16_t index = pt_ability_index(d->ab[i]);
        const char* effect = index ? pt_ability_effect(&abilities[index], generation) : NULL;
        snprintf(
            buf,
            sizeof(buf),
            "%s%s%s%s",
            d->ab[i],
            d->ab_hidden[i] ? " (HA)" : "",
            effect ? ": " : "",
            effect ? effect : "");
        pt_row_wrapped(r, 0, buf);
    }
}

// "Forms: A, B, C": the names of all of the Pokemon's forms.
static void pt_rows_form_list(PtRows* r, const char* formdata) {
    if(formdata == NULL) return;

    char buf[120] = "Forms: ";
    size_t n = strlen(buf);
    char token[PT_TOKEN_MAX];
    const char* cursor = formdata;
    bool first = true;
    while(pt_next_part_sep(&cursor, token, sizeof(token), '|')) {
        char* colon = strchr(token, ':');
        if(colon) *colon = '\0';
        int w = snprintf(buf + n, sizeof(buf) - n, "%s%s", first ? "" : ", ", token);
        if(w < 0 || (size_t)w >= sizeof(buf) - n) break;
        n += (size_t)w;
        first = false;
    }
    pt_row_wrapped(r, 0, buf);
}

static void pt_detail_rows(PtRows* r, const DetailModel* model) {
    char buf[48];
    if(model->has_form && model->form.label[0] != '\0') {
        snprintf(buf, sizeof(buf), "Form: %s", model->form.label);
        pt_row_wrapped(r, 0, buf);
    }
    // A form's note ("-> Perrserker: Lv28") replaces the evolution rows when it
    // says what the form evolves into; any other note goes below them.
    const char* note = model->has_form ? model->form.note : "";
    bool note_is_evolution = (note[0] == '-' && note[1] == '>');
    if(!note_is_evolution) pt_rows_evolution(r, model->evolves_to, model->evo_method);
    if(note[0] != '\0') pt_row_wrapped(r, 0, note);

    if(model->has_form) {
        pt_rows_abilities(r, &model->form, model->generation);
    } else {
        PtDefender own;
        pt_entry_abilities(model->entry, &own);
        own.ab_unknown = false;
        pt_rows_abilities(r, &own, model->generation);
    }
    pt_rows_form_list(r, model->formdata);
    if(model->matchup_hint) pt_row(r, "OK: Matchups");
}

static int pt_detail_row_count(const DetailModel* model) {
    PtRows r = {NULL, 0, 0};
    pt_detail_rows(&r, model);
    return r.row;
}

// Called by the GUI thread whenever the screen needs repainting.
static void pt_detail_draw_callback(Canvas* canvas, void* model_ptr) {
    const DetailModel* model = model_ptr;
    const PokemonEntry* e = model->entry;

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    if(!e) return;

    char buf[40];

    // Name (large) on the left, "position/total" (small) on the right.
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 2, 0, AlignLeft, AlignTop, e->name);

    canvas_set_font(canvas, FontSecondary);
    if(model->pick) {
        snprintf(buf, sizeof(buf), "OK: pick");
    } else {
        snprintf(buf, sizeof(buf), "%d/%d", model->position + 1, model->total);
    }
    canvas_draw_str_aligned(canvas, 126, 1, AlignRight, AlignTop, buf);

    // Dex number and type(s) (of the form being shown, if there is one).
    int t1 = model->has_form ? model->form.t1 : e->type1;
    int t2 = model->has_form ? model->form.t2 : (e->type2 == PT_NO_TYPE ? -1 : e->type2);
    if(t2 < 0) {
        snprintf(buf, sizeof(buf), "#%03d  %s", e->number, type_names[t1]);
    } else {
        snprintf(buf, sizeof(buf), "#%03d  %s/%s", e->number, type_names[t1], type_names[t2]);
    }
    canvas_draw_str_aligned(canvas, 2, 13, AlignLeft, AlignTop, buf);

    // The scrollable body: evolutions, then abilities.
    PtRows rows = {canvas, model->scroll, 0};
    pt_detail_rows(&rows, model);

    // Scroll bar along the right edge, only when there is more than fits.
    if(rows.row > PT_BODY_ROWS) {
        elements_scrollbar_pos(
            canvas,
            128,
            PT_BODY_Y,
            PT_BODY_ROWS * PT_ROW_H,
            (size_t)model->scroll,
            (size_t)(rows.row - PT_BODY_ROWS + 1));
    }

    // Arrows at the right end of the type line show which directions have
    // another entry (Left / Right).
    if(model->position > 0) {
        pt_draw_arrow(canvas, 110, 16, true);
    }
    if(model->position < model->total - 1) {
        pt_draw_arrow(canvas, 118, 16, false);
    }
}

// Copies the currently selected entry into the view's model, then asks the
// GUI to repaint (that's the 'true' in view_commit_model). A new entry always
// starts scrolled to the top.
static void pt_update_detail_model(PokemonTypesApp* app) {
    DetailModel* model = view_get_model(app->detail_view);
    const PokemonEntry* entry = &app->pokedex[PT_FI_ENTRY(app->filtered_indices[app->selected_position])];
    model->entry = entry;
    model->evolves_to = app->evo_pool + entry->evo_offset;
    model->evo_method = model->evolves_to + strlen(model->evolves_to) + 1;
    model->formdata = pt_entry_formdata(app, entry);
    // A Pokemon with forms shows its default form (the first) unless one was chosen.
    model->has_form = false;
    if(model->formdata) {
        int form = app->detail_form >= 0 ? app->detail_form : 0;
        model->has_form = pt_form_get(app, entry, form, &model->form);
    }
    model->generation = (int)(app->gen - generations) + 1;
    model->position = app->selected_position;
    model->total = app->filtered_count;
    model->scroll = 0;
    model->pick = app->pick_mode;
    model->matchup_hint = !app->pick_mode;
    view_commit_model(app->detail_view, true);
}

// Called for every button event while the detail screen is showing.
// Return true = "I handled it"; false = "not mine, pass it on".
//   Left / Right : previous / next entry
//   Up / Down    : scroll the body up / down (when it is longer than the screen)
static bool pt_detail_input_callback(InputEvent* event, void* context) {
    PokemonTypesApp* app = context;

    // Short press = one step; Repeat = key held down, so you can scroll fast.
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) {
        return false;
    }

    if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        int delta = (event->key == InputKeyLeft) ? -1 : 1;
        int new_position = app->selected_position + delta;
        if(new_position >= 0 && new_position < app->filtered_count) {
            app->selected_position = new_position;
            app->detail_form = PT_FI_FORM(app->filtered_indices[new_position]);
            app->detail_explicit = app->detail_form >= 0;
            pt_update_detail_model(app);
        }
        // At the first/last entry we still report "handled" and just do nothing.
        return true;
    }

    if(event->key == InputKeyOk) {
        if(event->type == InputTypeShort) {
            pt_defender_picked(app);
        }
        return true;
    }

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        DetailModel* model = view_get_model(app->detail_view);
        int max_scroll = pt_detail_row_count(model) - PT_BODY_ROWS;
        if(max_scroll < 0) max_scroll = 0;

        int new_scroll = model->scroll + ((event->key == InputKeyDown) ? 1 : -1);
        if(new_scroll < 0) new_scroll = 0;
        if(new_scroll > max_scroll) new_scroll = max_scroll;

        bool changed = (new_scroll != model->scroll);
        model->scroll = new_scroll;
        view_commit_model(app->detail_view, changed); // repaint only if it moved
        return true;
    }

    // Back (and everything else) is not ours. Returning false lets the
    // ViewDispatcher run pt_navigation_callback, which goes back a screen.
    return false;
}

static void pt_show_pokedex_detail(PokemonTypesApp* app) {
    pt_update_detail_model(app);
    app->step = StepPokedexDetail;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewDetail);
}

// --- Choosing the defender from the Pokedex ------------------------------------

static const PokemonEntry* pt_selected_entry(const PokemonTypesApp* app) {
    return &app->pokedex[PT_FI_ENTRY(app->filtered_indices[app->selected_position])];
}

// Appends "Type xN, Type xN" for the attack types listed in 'types' (a bit mask).
static void pt_cat_type_list(FuriString* out, const int* mult16, const bool* in_list, int count) {
    bool first = true;
    for(int t = 0; t < count; t++) {
        if(!in_list[t]) continue;
        char m[16];
        pt_mult_text(mult16[t], m, sizeof(m));
        furi_string_cat_printf(out, "%s%s %s", first ? "" : ", ", type_names[t], m);
        first = false;
    }
}

// The defensive profile of app->defender: what each attack type does to it,
// and what each of its abilities changes.
static void pt_show_matchups(PokemonTypesApp* app) {
    const GenerationInfo* gen = app->gen;
    int generation = pt_gen_number(app);
    const PtDefender* d = &app->defender;
    int n = gen->type_count;

    int base[TYPE_COUNT];
    for(int t = 0; t < n; t++) base[t] = pt_damage16(gen, generation, t, d->t1, d->t2, NULL);

    FuriString* body = furi_string_alloc();

    // Groups of attack types with the same multiplier, strongest first.
    bool done[TYPE_COUNT] = {false};
    for(;;) {
        int best = -1;
        for(int t = 0; t < n; t++) {
            if(done[t] || base[t] == 16) continue;
            if(best < 0 || base[t] > base[best]) best = t;
        }
        if(best < 0) break;
        char m[16];
        pt_mult_text(base[best], m, sizeof(m));
        furi_string_cat_printf(body, "%s:", base[best] ? m : "x0");
        bool first = true;
        for(int t = 0; t < n; t++) {
            if(done[t] || base[t] != base[best]) continue;
            furi_string_cat_printf(body, "%s%s", first ? " " : ", ", type_names[t]);
            first = false;
            done[t] = true;
        }
        furi_string_cat_printf(body, "\n");
    }
    if(furi_string_size(body) == 0) furi_string_cat_printf(body, "Everything does x1\n");

    // What each ability changes.
    for(int i = 0; i < d->ab_count; i++) {
        const PtMatchupRule* rule = pt_find_rule(d->ab[i]);
        if(!rule || generation < rule->from_gen) continue;
        int with[TYPE_COUNT];
        bool differs[TYPE_COUNT];
        bool any = false;
        for(int t = 0; t < n; t++) {
            with[t] = pt_damage16(gen, generation, t, d->t1, d->t2, rule);
            differs[t] = with[t] != base[t];
            any = any || differs[t];
        }
        if(any) {
            furi_string_cat_printf(body, "%s%s: ", d->ab[i], d->ab_hidden[i] ? " (HA)" : "");
            pt_cat_type_list(body, with, differs, n);
            furi_string_cat_printf(body, "\n");
        }
        if(rule->note) furi_string_cat_printf(body, "%s: %s\n", d->ab[i], rule->note);
    }

    char types[40];
    snprintf(
        types,
        sizeof(types),
        "%s%s%s%s%s",
        d->label[0] ? d->label : "",
        d->label[0] ? ": " : "",
        type_names[d->t1],
        d->t2 >= 0 ? "/" : "",
        d->t2 >= 0 ? type_names[d->t2] : "");

    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 1, AlignCenter, AlignTop, FontPrimary, d->name);
    widget_add_string_element(app->widget, 64, 13, AlignCenter, AlignTop, FontSecondary, types);
    widget_add_text_scroll_element(app->widget, 0, 24, 128, 40, furi_string_get_cstr(body));
    furi_string_free(body);

    app->step = StepMatchups;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewResult);
}

// Shows the result for the defender now stored in app->defender (or, when the
// Pokemon's page was opened by browsing, its matchups).
static void pt_defender_ready(PokemonTypesApp* app) {
    if(app->matchup_view) {
        pt_show_matchups(app);
        return;
    }
    app->defense_type1 = app->defender.t1;
    app->defense_type2 = app->defender.t2;
    app->defender_is_pokemon = true;
    app->pick_mode = false;
    pt_show_type_result(app);
}

// The "Pick a Form" list: one item per form of the selected Pokemon. The item id is the
// form's number in its FormData.
static void pt_show_form_pick(PokemonTypesApp* app) {
    const PokemonEntry* e = pt_selected_entry(app);
    const char* cursor = pt_entry_formdata(app, e);
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Pick a Form");
    char token[PT_TOKEN_MAX];
    uint32_t id = 0;
    while(pt_next_part_sep(&cursor, token, sizeof(token), '|')) {
        PtDefender f;
        pt_parse_form(token, &f);
        submenu_add_item(app->submenu, f.label, id++, pt_submenu_callback, app);
    }
    submenu_set_selected_item(app->submenu, (uint32_t)(app->detail_form > 0 ? app->detail_form : 0));
    app->step = StepFormPick;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// Makes the defender out of the form being shown on the detail screen (the default
// form, for a Pokemon with forms where none was picked).
static void pt_defender_from_detail(PokemonTypesApp* app) {
    const PokemonEntry* e = pt_selected_entry(app);
    if(pt_entry_formdata(app, e) &&
       pt_form_get(app, e, app->detail_form >= 0 ? app->detail_form : 0, &app->defender)) {
        return;
    }
    pt_defender_from_entry(e, &app->defender);
    snprintf(app->defender.name, sizeof(app->defender.name), "%s", e->name);
}

// A row of a Pokemon list was selected: its page, or the form list first when the
// Pokemon has forms (unless the row is one particular form).
static void pt_open_selected(PokemonTypesApp* app) {
    int form = PT_FI_FORM(app->filtered_indices[app->selected_position]);
    app->detail_form = form;
    app->detail_explicit = form >= 0;
    if(form < 0 && pt_entry_formdata(app, pt_selected_entry(app))) {
        pt_show_form_pick(app);
    } else {
        pt_show_pokedex_detail(app);
    }
}

// A form was chosen on the "Pick a Form" list.
static void pt_form_chosen(PokemonTypesApp* app, uint32_t index) {
    app->detail_form = (int)index;
    app->detail_explicit = true;
    if(app->pick_mode) {
        pt_defender_from_detail(app);
        app->matchup_view = false;
        pt_defender_ready(app);
    } else {
        pt_show_pokedex_detail(app);
    }
}

// OK on a Pokemon's page: choose it as the defender, or show its matchups.
static void pt_defender_picked(PokemonTypesApp* app) {
    if(app->pick_mode && !app->detail_explicit &&
       pt_entry_formdata(app, pt_selected_entry(app))) {
        pt_show_form_pick(app); // arrived by the arrow keys: choose which form
        return;
    }
    pt_defender_from_detail(app);
    app->matchup_view = !app->pick_mode; // browsing: matchups; choosing: pick
    pt_defender_ready(app);
}

// --- Main menu ---------------------------------------------------------

static void pt_populate_main_menu(PokemonTypesApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, MAIN_MENU_TITLE);
    submenu_add_item(app->submenu, "Type Effectiveness", 0, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "Pokedex Search", 1, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "Pokedex Browse", 2, pt_submenu_callback, app);
    submenu_add_item(app->submenu, "Memory Info", 3, pt_submenu_callback, app);
    app->step = StepMainMenu;
    view_dispatcher_switch_to_view(app->view_dispatcher, PtViewSubmenu);
}

// --- Shared submenu callback & navigation -------------------------------

static void pt_submenu_callback(void* context, uint32_t index) {
    PokemonTypesApp* app = context;
    switch(app->step) {
    case StepGenSelect:
        app->gen_choice = (int)index;
        pt_show_gen_confirm(app);
        break;
    case StepMainMenu:
        if(index == 0) {
            pt_populate_type_attack(app);
        } else if(index == 3) {
            pt_show_main_mem(app);
        } else if(!app->pokedex_loaded) {
            pt_show_pokedex_error(app);
        } else if(index == 1) {
            pt_show_pokedex_search(app);
        } else {
            pt_populate_browse_by(app);
        }
        break;
    case StepTypeAttack:
        app->attack_type = (int)index;
        pt_populate_defense_options(app);
        break;
    case StepDefenseOptions:
        app->options_choice = (int)index;
        if(index == 0) {
            app->defender_is_pokemon = false;
            pt_populate_type_defense1(app);
        } else if(index == 1) {
            app->pick_mode = true;
            if(!app->pokedex_loaded) {
                pt_show_pokedex_error(app);
            } else {
                pt_populate_defender_source(app);
            }
        } else {
            app->ignore_abilities = !app->ignore_abilities;
            pt_populate_defense_options(app);
        }
        break;
    case StepDefenderSource:
        if(index == 0) {
            pt_show_pokedex_search(app);
        } else {
            pt_start_pokedex_browse(app);
        }
        break;
    case StepFormPick:
        pt_form_chosen(app, index);
        break;
    case StepTypeDefense1:
        app->defense_type1 = (int)index;
        pt_populate_type_defense2(app);
        break;
    case StepTypeDefense2:
        app->defense_type2 = (index == TYPE_COUNT) ? -1 : (int)index;
        app->defender_is_pokemon = false;
        pt_show_type_result(app);
        break;
    case StepPokedexGroups:
        pt_open_pokedex_group(app, (int)index);
        break;
    case StepPokedexResults:
        if(index == PT_ID_PREV_PAGE) {
            app->selected_position = (app->selected_position / PT_PAGE - 1) * PT_PAGE;
            pt_populate_pokedex_results(app);
        } else if(index == PT_ID_NEXT_PAGE) {
            app->selected_position = (app->selected_position / PT_PAGE + 1) * PT_PAGE;
            pt_populate_pokedex_results(app);
        } else {
            app->selected_position = (int)index;
            pt_open_selected(app);
        }
        break;
    case StepBrowseBy:
        if(index == 0) {
            pt_start_pokedex_browse(app);
        } else if(index == 1) {
            pt_populate_browse_types(app);
        } else {
            pt_populate_browse_letters(app);
        }
        break;
    case StepBrowseType:
        pt_open_browse_list(app, 1, (int)index);
        break;
    case StepBrowseLetter:
        app->browse_letter = (char)index;
        app->browse_arg = 0;
        pt_populate_browse_abilities(app);
        break;
    case StepBrowseAbility:
        pt_open_browse_list(app, 2, (int)index);
        break;
    default:
        break;
    }
}

static bool pt_navigation_callback(void* context) {
    PokemonTypesApp* app = context;
    switch(app->step) {
    case StepGenConfirm:
        pt_populate_gen_select(app);
        return true;
    case StepGenSelect:
        pt_show_title(app);
        return true;
    case StepGenMem:
        pt_show_gen_confirm(app);
        return true;
    case StepMemError:
        pt_populate_gen_select(app);
        return true;
    case StepMainMem:
        pt_populate_main_menu(app);
        return true;
    case StepMainMenu:
        pt_populate_gen_select(app); // leave the generation (this frees its data)
        return true;
    case StepTypeAttack:
    case StepBrowseBy:
        pt_populate_main_menu(app);
        return true;
    case StepBrowseType:
    case StepBrowseLetter:
        pt_populate_browse_by(app);
        return true;
    case StepBrowseAbility:
        pt_populate_browse_letters(app);
        return true;
    case StepMatchups:
        pt_show_pokedex_detail(app);
        return true;
    case StepPokedexSearch:
        if(app->pick_mode) {
            pt_populate_defender_source(app);
        } else {
            pt_populate_main_menu(app);
        }
        return true;
    case StepDefenderSource:
        pt_populate_defense_options(app);
        return true;
    case StepPokedexError:
        if(app->pick_mode) {
            pt_populate_defense_options(app);
        } else {
            pt_populate_main_menu(app);
        }
        return true;
    case StepDefenseOptions:
        pt_populate_type_attack(app);
        return true;
    case StepFormPick:
        pt_populate_pokedex_results(app); // back to the list the Pokemon came from
        return true;
    case StepTypeDefense1:
        pt_populate_defense_options(app);
        return true;
    case StepTypeDefense2:
        pt_populate_type_defense1(app);
        return true;
    case StepTypeResult:
        if(app->defender_is_pokemon) {
            pt_populate_defense_options(app); // a new search
        } else {
            pt_populate_type_defense2(app);
        }
        return true;
    case StepPokedexGroups:
        if(app->pick_mode) {
            pt_populate_defender_source(app);
        } else {
            pt_populate_browse_by(app);
        }
        return true;
    case StepPokedexResults:
        if(app->browse_mode && app->browse_kind == 1) {
            pt_populate_browse_types(app);
        } else if(app->browse_mode && app->browse_kind == 2) {
            pt_populate_browse_abilities(app);
        } else if(app->browse_mode) {
            if(app->gen->group_count > 1) {
                pt_populate_pokedex_groups(app); // back to the list of groups
            } else if(app->pick_mode) {
                pt_populate_defender_source(app); // this generation has a single group
            } else {
                pt_populate_browse_by(app); // this generation has a single group
            }
        } else {
            pt_show_pokedex_search(app); // a search started from the keyboard
        }
        return true;
    case StepPokedexDetail:
        // A Pokemon opened through its form list goes back to that list.
        if(pt_entry_formdata(app, pt_selected_entry(app)) == NULL ||
           PT_FI_FORM(app->filtered_indices[app->selected_position]) >= 0) {
            pt_populate_pokedex_results(app);
        } else {
            pt_show_form_pick(app);
        }
        return true;
    case StepTitle:
    default:
        return false; // let the dispatcher stop and exit the app
    }
}

// --- App lifecycle -------------------------------------------------------

static PokemonTypesApp* pokemon_types_app_alloc(void) {
    PokemonTypesApp* app = malloc(sizeof(PokemonTypesApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->submenu = submenu_alloc();
    app->widget = widget_alloc();
    app->text_input = text_input_alloc();

    // Custom detail screen: allocate the view, give it room for a DetailModel
    // (the "Locking" type makes drawing and input safe to run on different
    // threads), and register our draw + input handlers.
    app->detail_view = view_alloc();
    view_allocate_model(app->detail_view, ViewModelTypeLocking, sizeof(DetailModel));
    view_set_context(app->detail_view, app);
    view_set_draw_callback(app->detail_view, pt_detail_draw_callback);
    view_set_input_callback(app->detail_view, pt_detail_input_callback);

    app->title_view = view_alloc();
    view_allocate_model(app->title_view, ViewModelTypeLocking, sizeof(TitleModel));
    view_set_context(app->title_view, app);
    view_set_draw_callback(app->title_view, pt_title_draw_callback);
    view_set_input_callback(app->title_view, pt_title_input_callback);

    app->saved_gen = -1;
    app->gen_choice = 0;
    app->gen = NULL;

    app->attack_type = 0;
    app->defense_type1 = 0;
    app->defense_type2 = -1;
    app->defender_is_pokemon = false;
    app->pick_mode = false;
    app->ignore_abilities = false;
    app->options_choice = 0;
    memset(&app->defender, 0, sizeof(app->defender));

    // The Pokedex arrays are allocated later, when a generation is confirmed.
    app->pokedex = NULL;
    app->filtered_indices = NULL;
    app->evo_pool = NULL;
    app->evo_pool_capacity = 0;
    app->evo_pool_used = 0;
    app->pokedex_capacity = 0;
    app->pokedex_count = 0;
    app->pokedex_loaded = false;
    app->filtered_count = 0;
    app->selected_position = 0;
    app->browse_mode = false;
    app->group_choice = 0;
    app->list_title[0] = '\0';
    app->browse_kind = 0;
    app->browse_arg = 0;
    app->browse_letter = 'A';
    app->matchup_view = false;
    app->detail_form = -1;
    app->detail_explicit = false;
    app->search_buffer[0] = '\0';

    view_dispatcher_add_view(app->view_dispatcher, PtViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(app->view_dispatcher, PtViewResult, widget_get_view(app->widget));
    view_dispatcher_add_view(
        app->view_dispatcher, PtViewTextInput, text_input_get_view(app->text_input));
    view_dispatcher_add_view(app->view_dispatcher, PtViewDetail, app->detail_view);
    view_dispatcher_add_view(app->view_dispatcher, PtViewTitle, app->title_view);

    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, pt_navigation_callback);

    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    return app;
}

static void pokemon_types_app_free(PokemonTypesApp* app) {
    pt_unload_generation(app);

    view_dispatcher_remove_view(app->view_dispatcher, PtViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, PtViewResult);
    view_dispatcher_remove_view(app->view_dispatcher, PtViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, PtViewDetail);
    view_dispatcher_remove_view(app->view_dispatcher, PtViewTitle);

    submenu_free(app->submenu);
    widget_free(app->widget);
    text_input_free(app->text_input);
    view_free_model(app->detail_view); // we allocated the model, so we free it
    view_free(app->detail_view);
    view_free_model(app->title_view);
    view_free(app->title_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t pokemon_types_app(void* p) {
    UNUSED(p);
    PokemonTypesApp* app = pokemon_types_app_alloc();

    pt_show_title(app); // the first screen: the title page

    view_dispatcher_run(app->view_dispatcher);

    pokemon_types_app_free(app);
    return 0;
}
