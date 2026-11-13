#pragma once

// Voice - modded voices on Lazy Followers followers (NG 1.2, user: "Sofia" voice). A follower line (Hello,
// Goodbye, Follow, Wait, Trade, Part ways, Favor) is swapped for a matching line of the voice's own mod, so
// it is spoken in that voice. Only the text + audio are swapped: the follower's own line keeps its scripts.

namespace Voice
{
    void Install();                            // kDataLoaded: hooks the game's "build a dialogue line" calls
    void Prepare(RE::BGSVoiceType* a_voice);   // main thread: collects the voice mod's lines (once per voice)
    bool IsModded(const RE::BGSVoiceType* a_voice);                  // NG 1.2: not a base-game voice
    std::vector<RE::FormID> PluginNPCs(RE::BGSVoiceType* a_voice);   // NG 1.2: main thread: the NPCs its plugin gives it (Prepare)
    void StartOwnerScan();                                          // NG: kDataLoaded: background read of the voice plugins (FileOwnedVoices)
    std::unordered_set<RE::FormID> FileOwnedVoices();               // NG: modded voices a unique NPC has in its plugin file (empty until the scan is done)
}
