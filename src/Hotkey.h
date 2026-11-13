#pragma once

// Hotkey - the Settings page's hotkeys (open / close the Lazy Followers window, summon the selected
// follower, summon all followers, save the player's preset, open the ticked actor's inventory,
// open / close the Nearby Actors window), read from Windows on its own thread like Actor Control's hotkeys.

#include "LF.h"

namespace Hotkey
{
    enum class Key
    {
        kOpenWindow,
        kSummon,
        kSummonAll,
        kSavePreset,
        kOpenInventory,  // NG (user): the inventory of the actor ticked in Settings > Nearby Actors
        kNearbyWindow    // NG (user): opens / closes the Nearby Actors window
    };

    void        Start();                               // kPostLoad: starts the key reader thread
    void        BeginCapture(Key a_key);               // a Settings key button: wait for a key for a_key
    void        CancelCapture();
    bool        Capturing();                           // waiting for a key (any of them)
    bool        Capturing(Key a_key);                  // waiting for a key for a_key
    std::string Name(const LF::KeyBinding& a_binding); // "Ctrl + F7", "(none)"
    void        FixShared();                           // in game: a key on several hotkeys stays only on the first row
}
