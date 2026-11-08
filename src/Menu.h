#pragma once

// Menu - the "Lazy Followers" section in SKSE Menu Framework (Followers, Stats, Spells, Combat Styles,
// Settings), which replaces Lazy Followers' MCM pages.

namespace Menu
{
    void Register();      // kPostLoad: adds the pages + the Lazy Followers window (needs SKSEMenuFramework.dll)
    void ToggleWindow();  // the Open Window hotkey: opens / closes the Lazy Followers window
}
