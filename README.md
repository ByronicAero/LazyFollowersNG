# Lazy Followers NG

An SKSE plugin that replaces [Lazy Followers](https://www.nexusmods.com/skyrimspecialedition/mods/43312) 0.42.3 by [LazyGirl](https://www.nexusmods.com/profile/LazyGirl?gameId=1704): its Papyrus scripts and MCM are now C++ and **SKSE Menu Framework** pages.
It uses its own modified copy of `LazyFollowers.esp` and the same files and folders as the original mod, so presets, NPC files and the config carry over.

Version 1.3.0. Built and tested on Skyrim **AE 1.6.1170**. It uses CommonLibSSE-NG with Address Library, so other SE / AE versions may load it, but they are not tested, and two parts only run on 1.6.1170: the talk-menu part of modded follower voices (greetings / answers in the talk menu) and clearing a taken-over NPC's race-change face on Remove.

## Requirements

| Mod | Version (tested) |
|---|---|
| Skyrim Special Edition / Anniversary Edition | 1.6.1170 |
| SKSE64 | 2.2.6 |
| Address Library for SKSE Plugins | 13.0 (All in one, AE) |
| SKSE Menu Framework | 3.23 |
| RaceMenu | 0.4.20 (AE) |

Optional:

| Mod | Version (tested) | For |
|---|---|---|
| Erin Race | 1.4.28 (its interface version 6 or newer) | the Erin Preset row (Erin race extra presets) |

The original Lazy Followers mod is **not** needed: this mod brings its own esp.
No longer needed: SkyUI / MCM, JContainers, PapyrusUtil, powerofthree's Papyrus Extender, Mu Dynamic NPC Replacer.

## Features

**Pages** (SKSE Menu Framework section "Lazy Followers", or their own window with a hotkey):

- **Followers**
  - The Follower list shows only followers in use: name and base form ID, searchable. Add Follower takes the next free slot.
  - Distance and location of the selected follower.
  - Appearance: name, preset, race and Female / Male / Other Voice, all searchable drop-downs. Picking a preset switches Race to the race it was made for. Apply Settings applies the preset, then brings the follower to you (not when it is already next to you).
  - Erin Preset (with the Erin race installed, only for Erin-race followers): loads an Erin race extra preset (extra tints, eye colour set, skin, head-part adjust) on the follower.
  - Keep in New Game (with Pre Summon): the follower comes back in a new game once RaceMenu closes.
  - NPC Preset, Actions (Summon, Teleport, Remove, Copy Kit with Auto Equip and Include Equipment), Apply to All / Summon All, Combat Style and Class.
- **Stats**: stats and skills, each with Reset (back to the value before you changed it) and Reset All.
- **Spells**: a folder per magic school with search, and a list of added spells (right-click removes one).
- **Combat Styles**: the 10 Lazy Followers combat styles with all their values, and a Follower section that opens that follower's style.
- **Settings**:
  - Nearby Actors with Save Actor Preset: saves any nearby actor's RaceMenu look and face tint, no console needed. The actor selected in the console, or else the actor you look at, is ticked by itself.
  - RaceMenu Mode, behaviour options, Config / NPC data files, logging.
  - Presets: one table of all RaceMenu presets (search, tick any number). Right-click: Hide (its .jslot, .dds and .nif are renamed to .hidden, nothing deleted), Unhide, Delete. Hidden ones are greyed out.
  - Hotkeys (each with Clear and Reset; one key per hotkey): open the pages as tabs in a window of their own; summon the selected follower; summon all followers; save your own look as a preset (asks a name, like `skee preset-save`); open the inventory of the ticked actor (an NPC's as a trade window, your own as normal; the hotkey again closes it); open Nearby Actors in a window of its own.

Stats, Combat Styles and Settings have a search box at the top.

**Modded follower voices**

- A follower given the voice type of a voiced-follower mod (for example Sofia or Severa) says that mod's own lines: greetings, Follow / Wait / Trade / Part ways answers and its "Can I talk to you?" topics. Lines the mod has no recording for are silenced.
- Override: with that voice picked, the mod's real NPC can take the follower's place. She gets the follower's preset, race and name, and keeps her own voice, quests and dialogue. Stats, spells, combat style and class can be set on her like on a slot follower. Remove gives back her own look, name, outfit, style and class and sends her back to her own place. Override can move from one follower to another.

**Changes from the original mod**

- 100 follower slots; any slot can be male or female (the sex follows the applied preset).
- New races and voices show up right away (the old lists lived in the save and went out of date).
- "Save Actor Preset" can no longer crash the game (the old `skee preset-save` console route could).
- A body preset applied later (OBody etc.) is kept: the preset's body is only applied by Apply Settings / Apply to All / NPC Preset, not on every load.
- Settings are global (one file for every save), like other SKSE Menu Framework mods.
- The Shouts page is left out (the MCM itself warned it could corrupt saves).

## The esp

`data/LazyFollowers.esp` is LazyGirl's original esp, modified for this plugin:

- all Papyrus scripts removed (the DLL does their work)
- 20 follower slots added (100 in all)
- ESL-flagged (ESPFE, takes no load order slot): its form IDs were renumbered to 0x800-0x8FD

The file name is unchanged, so it replaces the original esp. Because the form IDs changed, a game save with followers already in slots does not carry them over: export them to NPC files first and load them back after switching. NPC files and the co-save written with the old esp are translated to the new IDs. It is used and modified under the original mod's permissions (modify and re-release with credit). The build packs it into the zip.

## Installing

1. If you have the old Lazy Followers installed, disable it in your mod manager. Keep its Overwrite / output files: presets, NPC files and the config live there.
2. Install the zip (the plugin DLL plus the modified esp).

Existing saves: the old scripts kept each follower's preset and name in the save, so that part is lost. Re-apply presets on the Followers page, or save your followers with the old MCM's "Save each NPC separately" before switching and load them with NPC Preset.

Files the mod writes (paths in `Data`; with MO2 new files go to Overwrite): `SKSE/Plugins/LazyFollowers/LazyFollowersNGSettings.json` (settings + hotkeys) and `LazyFollowersNGKeep.json` (Keep in New Game), the old mod's config / NPC files in the same folder, presets in `SKSE/Plugins/CharGen/Exported` + `Textures/CharGen/Exported`, and helper files in `SKSE/Plugins/CharGen/LazyFollowersNG` and `SKSE/Plugins/LazyFollowersNG`. Log: `Documents\My Games\Skyrim Special Edition\SKSE\LazyFollowersNG.log`.

## Building

Needs:

- Visual Studio 2022 or 2026 (or its Build Tools) with "Desktop development with C++" (includes CMake and Ninja)
- Git for Windows
- [vcpkg](https://github.com/microsoft/vcpkg), with the `VCPKG_ROOT` environment variable pointing to it:
  ```
  git clone https://github.com/microsoft/vcpkg
  vcpkg\bootstrap-vcpkg.bat -disableMetrics
  set VCPKG_ROOT=<full path to that vcpkg folder>
  ```

Then, in an **x64 Native Tools Command Prompt** opened in this folder:

```
cmake --preset release
cmake --build --preset release
```

CommonLibSSE-NG, spdlog, nlohmann-json, rapidcsv and zlib are downloaded on the first run. The ready-to-install mod folder (`SKSE\Plugins\LazyFollowersNG.dll` + `LazyFollowers.esp`) is `build/release/package/`; zip its contents to install it with a mod manager. The folder path must not contain `&`.

## Layout

| Folder | What |
|---|---|
| `src` | `main.cpp` (SKSE messages), `LF.cpp` (follower slots, co-save, actions, take-over, Erin preset, Open Inventory), `Files.cpp` (JSON files, presets), `Papyrus.cpp` (safe Papyrus calls), `Tint.cpp` (face tint to .dds), `Menu.cpp` (the pages and the windows), `Hotkey.cpp` (hotkeys), `Voice.cpp` (modded follower voices) |
| `include` | precompiled header, SKSE Menu Framework header |
| `data` | the modified `LazyFollowers.esp` (packed into the zip) |

## Credits

- [LazyGirl](https://www.nexusmods.com/profile/LazyGirl?gameId=1704), creator of [Lazy Followers](https://www.nexusmods.com/skyrimspecialedition/mods/43312): the original mod, its esp, and the scripts and MCM this plugin re-implements.
- CommonLibSSE-NG, SKSE, SKSE Menu Framework, RaceMenu, Erin Race (its extra preset interface).
