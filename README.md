# Lazy Followers NG

An SKSE plugin that replaces [Lazy Followers](https://www.nexusmods.com/skyrimspecialedition/mods/43312) 0.42.3 by [LazyGirl](https://www.nexusmods.com/profile/LazyGirl?gameId=1704): its Papyrus scripts and MCM are now C++ and **SKSE Menu Framework** pages.
It uses its own modified copy of `LazyFollowers.esp` and the same files and folders as the original mod, so presets, NPC files and the config carry over.

Skyrim **AE 1.6.1170** (CommonLibSSE-NG, so it should also work on SE / other AE versions with Address Library).

## Requirements

- SKSE
- Address Library for SKSE Plugins
- SKSE Menu Framework
- RaceMenu

The original Lazy Followers mod is **not** needed: this mod brings its own esp.
No longer needed: SkyUI / MCM, JContainers, PapyrusUtil, powerofthree's Papyrus Extender.

## Features

**Pages** (SKSE Menu Framework section "Lazy Followers", or their own window with a hotkey):

- **Followers**
  - The Follower list shows only followers in use: name and base form ID, searchable. Add Follower takes the next free slot.
  - Distance and location of the selected follower.
  - Appearance: name, preset, race and Female / Male / Other Voice, all searchable drop-downs. Apply Settings applies the preset, then brings the follower to you.
  - NPC Preset, Actions (Summon, Teleport, Reset / Dismiss, Copy Kit), Apply to All / Summon All, Combat Style and Class.
- **Stats**: stats and skills, each with Reset (back to the value before you changed it) and Reset All.
- **Spells**: a folder per magic school with search, and a list of added spells (right-click removes one).
- **Combat Styles**: the 10 Lazy Followers combat styles with all their values, and a Follower section that opens that follower's style.
- **Settings**:
  - Nearby Actors with Save Actor Preset: saves any nearby actor's RaceMenu look and face tint, no console needed.
  - RaceMenu Mode, behaviour options, Config / NPC data files, logging.
  - Hotkey: opens the pages as tabs in a window of their own.

Stats, Combat Styles and Settings have a search box at the top.

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

File name and form IDs are unchanged, so it replaces the original esp and existing saves keep their follower slots. It is used and modified under the original mod's permissions (modify and re-release with credit). The build packs it into the zip.

## Installing

1. If you have the old Lazy Followers installed, disable it in your mod manager. Keep its Overwrite / output files: presets, NPC files and the config live there.
2. Install the zip (the plugin DLL plus the modified esp).

Existing saves: the old scripts kept each follower's preset and name in the save, so that part is lost. Re-apply presets on the Followers page, or save your followers with the old MCM's "Save each NPC separately" before switching and load them with NPC Preset.

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

CommonLibSSE-NG, spdlog and nlohmann-json are downloaded on the first run. The ready-to-install mod folder (`SKSE\Plugins\LazyFollowersNG.dll` + `LazyFollowers.esp`) is `build/release/package/`; zip its contents to install it with a mod manager. The folder path must not contain `&`.

## Layout

| Folder | What |
|---|---|
| `src` | `main.cpp` (SKSE messages), `LF.cpp` (follower slots, co-save, actions), `Files.cpp` (JSON files, presets), `Papyrus.cpp` (safe Papyrus calls), `Tint.cpp` (face tint to .dds), `Menu.cpp` (the pages and the window), `Hotkey.cpp` (window hotkey) |
| `include` | precompiled header, SKSE Menu Framework header |
| `data` | the modified `LazyFollowers.esp` (packed into the zip) |

## Credits

- [LazyGirl](https://www.nexusmods.com/profile/LazyGirl?gameId=1704), creator of [Lazy Followers](https://www.nexusmods.com/skyrimspecialedition/mods/43312): the original mod, its esp, and the scripts and MCM this plugin re-implements.
- CommonLibSSE-NG, SKSE, SKSE Menu Framework, RaceMenu.
