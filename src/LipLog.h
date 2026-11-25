#pragma once

// LipLog - diagnostics only (146b, user: followers say a line twice, the second time without lip sync, after
// alt-tab or right after a load). Logs every voice line start / lip request / lip stop of our followers and of
// NPCs near the player (vanilla control), samples their voice sound + lip state while they talk, and logs when the
// game window loses / gets focus. Changes nothing in the game. RE: Docs/lip-sync-re.md.

namespace LipLog
{
    void Install();  // kDataLoaded: hooks the game's voice-line start / lip calls (1.6.1170 only) + starts the sampler
}
