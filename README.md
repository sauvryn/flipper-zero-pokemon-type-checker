# Pokemon Type Checker for Flipper Zero

> **AI disclosure:** I am still new to coding, and this app was built using coding assistance from Claude.AI. Under my direction - and over the course of many steps (and days) - Claude wrote and revised most of the C source and the data-generation scripts. I gained practice with coding by editing the generated code to attempt simple changes and additions, which Claude.AI gave me instructions and explanations for. I directed the design and chose the features, and briefly tested every build on my real Flipper Zero, which is currently running the latest release of Momentum Firmware (mntm-012). I used Claude.AI to check the Pokemon data against reputable sources (which are credited in this document). While all of the data has not yet been verified entry by entry, manual verification is ongoing and will take some time due to the scope of the app (which ultimately went far beyond the Gen 1-4 I'd originally planned). Please feel free to report anything that is incorrect (see [Known limitations](#known-limitations)).

**Description:** A Pokemon type-effectiveness calculator and offline bare-bones Pokedex ("bare-bones": as in, it doesn't contain images or any flavor text) for the Flipper Zero, covering Generations 1-9 (1025 Pokemon). This is an unofficial, fan-made tool that I designed for my own personal use so that I could have a mobile, offline, digital, non-smartphone information resource while playing Pokemon games (mostly the older ones right now - I'm in a retro gaming phase, haha). It is not affiliated with or endorsed by Nintendo, Game Freak, Creatures or The Pokemon Company.

## Features

- **Type Effectiveness**: pick an attack type, a defense type(s) or specific defending Pokemon, and see what hits for 4x, 2x, 1x, 1/2x, 1/4x or 0x. The specific defender Pokemon results include extra information for their possible abilities that affect damage nullifactions or multipliers such as Levitate, Thick Fat, Fluffy, Dry Skin and Tera Shell.
- **Pokedex Search**: type part or all of a name to find matching Pokemon.
- **Pokedex Browse**: browse by Pokedex numbers (in Generational groups), by type, or by ability.
- **Detail pages**: types, evolution line and method, and abilities with a short combat effect summary, if any (hidden abilities are marked with "HA"). Abilities that do not affect combat are left undefined, as that is beyond the scope of this app.
- **Matchups**: from any Pokemon's page, access the Matchups page to see what it is weak or resistant to.
- **Alternate forms**: Pokemon with different forms (regional forms, Mega Evolutions, Rotom appliances, Ogerpon masks, Arceus and Silvally types and so on) show a **Pick a Form** screen when selected from the generic name (e.g.- Basculin). Each form entry has its own types and abilities, and the specific form will appear in search lists when applicable to searching by type or by ability (e.g.- a search for the "Reckless" ability will include "Basculin (Red-Striped)" in the search results but not the other two variants).
- **Generation aware**: the type chart, available Pokemon and forms follow the rules of the generation you choose (for example, no Fairy type in Gens 1-5 and no Steel or Dark in Gen 1). Furthermore, abilities that gained a combat effect in later generations will lack that effect in the appropriate former generations. There were some type effectiveness differences in earlier games, specifically: Gen 1 → Gen 2	(Ice → Fire is now 0.5x, Poison → Bug is now 1x, Bug → Poison is now 0.5x, and Ghost → Psychic is now 2x) and Gen 2–5 → Gen 6 (Ghost → Steel and Dark → Steel are now 1x, and Fairy is added). See https://pokemondb.net/type/old and https://pokemondb.net/type/ for reference.
- **Memory Info**: a setting implemented during testing that shows free memory, because the biggest generations use a lot of the Flipper's RAM. I left it in because the information may be of interest.

## What you need

- A Flipper Zero with an SD card.
- Firmware with the standard external-app API. It was developed and tested on **Momentum firmware (mntm-012, API 87.1)**. It should build for other firmwares that offer the same API, but I have not tested any others.
- [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) to build it (see below). Python 3 is needed to install `ufbt`.

## Installing

### Option A: download a release
1. Ensure you are running the latest release of Momentum Firmware (mntm-012) on your Flipper Zero.
2. Download `pokemon_types_fap.zip` and `pokedex_gen.zip` from the [Releases](../../releases) page. Unzip both.
3. Copy `pokemon_types.fap` to `SD Card/apps/Tools/` on the Flipper.
4. Unzip the data and copy all the `pokedex_gen*.csv` files to `SD Card/apps_data/pokemon_types/` (create the folder if it does not exist).
5. Access on your Flipper Zero via Apps -> Tools -> Pokemon Type Checker.

### Option B: build it yourself
1. Install `ufbt`:
   ```
   py -m pip install --upgrade ufbt        (Windows)
   python3 -m pip install --upgrade ufbt   (Linux / macOS)
   ```
2. Clone this repo and go into it:
   ```
   git clone https://github.com/sauvryn/Flipper-Zero-Pokemon-Type-Checker.git
   cd flipper-zero-pokemon-type-checker
   ```
3. Connect your Flipper by USB (close qFlipper and the Flipper lab website first, since they hold the serial port) and run:
   ```
   py -m ufbt launch        (Windows)
   ufbt launch              (Linux / macOS)
   ```
   This compiles the app, copies it to the Flipper and starts it. To only build, run `ufbt` on its own. The result is `~/.ufbt/build/pokemon_types.fap` (on Windows, `C:\Users\<you>\.ufbt\build\pokemon_types.fap`).
4. Copy the `data/pokedex_gen*.csv` files to `SD Card/apps_data/pokemon_types/` on the Flipper.

> `ufbt` downloads the SDK that matches your firmware channel. If the build fails with API errors, update `ufbt` and check that it targets the same firmware you run: `ufbt update`, or `ufbt update -t <channel>`.

## The data files

The app reads one CSV file per generation, loaded only when you pick that generation (this is how it fits in the Flipper's memory). Without the CSV files, the Type Effectiveness calculator still works, but the Pokedex shows a message telling you which file to copy.

Format (one Pokemon per line):

```
Number,Name,Type1,Type2,EvolvesTo,EvoMethod,Ability1,Ability2,HiddenAbility,Forms,FormData
```

Forms are described in the last column as `|`-separated entries of the form `Label:Types[:Abilities[:Note]]`:
- `Types` is a string of type letters (`A` Normal, and so on up to `R` Fairy); empty means "same as the base Pokemon".
- `Abilities` are `/`-separated, a trailing `^` marks a Hidden Ability, and empty means "same as the base Pokemon".
- The first entry is the default form.
- The `Forms` column is kept for compatibility and is no longer used.

If you want to correct or extend the data, edit the CSVs in a spreadsheet or text editor and copy them to the SD card. No rebuild is needed unless you add a new ability name that the app does not know (ability effect text lives in the `abilities[]` table in `pokemon_types.c`). Note that the evolution/form text pool size per generation (`evo_pool_size`) is set in the source, so large additions to the data may need a bigger number there.

## Known limitations

- **Newest Mega Evolutions:** the abilities of the Megas introduced in *Mega Dimension* (Zygarde, Heatran, Darkrai, Magearna, Zeraora, Tatsugiri) are not published in any source I could reach. They are **assumed to match the base forms** until verified in-game. Some of their types (for example Mega Zygarde) are also unconfirmed.
- Data was assembled from public sources (see Credits) and may contain errors. Issues and corrections are welcome.
- Gen 8 and Gen 9 files are large. Use **Memory Info** if you run other apps or a heavily loaded firmware.
- Tested only on Momentum firmware.

## Future Updates
- Generation 10 is on its way, as we all know, but I am unsure at this time if all of the new data will fit. I *do* plan to attempt to update the app for Gen 10, so I'll post progress on that once all of the new pokemon data is available.

## Credits and data sources

- Pokemon data (forms, types, abilities) is derived from the open [PokeAPI](https://github.com/PokeAPI/pokeapi) data, and supplemented by checks against Bulbapedia, Serebii, and PokemonDB.
- Built with the Flipper Zero SDK (`ufbt`).
- Written with coding assistance from Claude.AI (see the disclosure at the top).

Pokemon and Pokemon character names are trademarks of Nintendo, Creatures Inc. and Game Freak Inc. This project includes NO official artwork, sprites or game assets.

## License

The source code is released under the MIT License (see `LICENSE`). The license covers the code only. It does not grant any rights to Pokemon names, trademarks or other intellectual property.
