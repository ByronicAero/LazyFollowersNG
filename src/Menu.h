#pragma once

// Menu - the "Lazy Followers" section in SKSE Menu Framework (Followers, Stats, Spells, Combat Styles,
// Settings), which replaces Lazy Followers' MCM pages.

namespace Menu
{
    void Register();      // kPostLoad: adds the pages + the Lazy Followers window (needs SKSEMenuFramework.dll)
    void ToggleWindow();  // the Open Window hotkey: opens / closes the Lazy Followers window
    void OpenSavePresetDialog();  // the Save Player Preset hotkey: the small "preset name" window
    void OpenPickedInventory();   // the Open Inventory hotkey: the inventory of the actor ticked in Nearby Actors
    void ToggleNearbyWindow();    // the Nearby Actors Window hotkey: Nearby Actors in a window of its own
}
