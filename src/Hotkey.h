#pragma once

// Hotkey - the Settings page's hotkey (open / close the Lazy Followers window), read from Windows on its
// own thread like Actor Control's hotkeys.

#include "LF.h"

namespace Hotkey
{
    void        Start();                               // kPostLoad: starts the key reader thread
    void        BeginCapture();                        // the Settings key button: wait for a key
    void        CancelCapture();
    bool        Capturing();                           // waiting for a key
    std::string Name(const LF::KeyBinding& a_binding); // "Ctrl + F7", "(none)"
}
