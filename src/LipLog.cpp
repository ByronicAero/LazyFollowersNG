// LipLog - diagnostics for "follower says a line twice, the second time without lip sync" (146b, user: after
// alt-tab or right after a load, every voice, only Lazy Followers followers). Changes nothing in the game: the
// game's own functions are called unchanged, we only write log lines ("Lip: ..." in LazyFollowersNG.log).
// RE (SkyrimSE 1.6.1170, Docs/lip-sync-re.md):
//   37542 = start a voice line (actor, voice path, sound handle out, ..., arg 12 = background lip load) -> float length
//   37545 = lip request only (actor, voice path, background)      39376 = stop the lip (AIProcess, actor)
//   HighProcessData +0x2FC soundHandles[0] (voice), +0x3B8 lip entry (state = bits 28-30 of entry+0x0C,
//   LipSynchAnim* at entry+0x28), +0x3C0 lip timer (float, game frames, not the sound position).
// Every direct call of the three in the game's code is hooked (the caller is named in the log). Who is logged:
// our followers (base NPC from LazyFollowers.esp) and anyone within 2000 units of the player (vanilla control).
// While such an actor talks, one sample line on every change or once a second (10 checks a second). The window
// focus and a stalled main thread (game paused while alt-tabbed) are logged from our own thread.
// (146c) Human faces: the face's own animation data (BSFaceGenAnimationData, read every frame by its update 26598)
// holds its own lip entry at +0x228 (37542 hands the process lip over there, but only for the player; 26591 frees
// it) and a freeze flag at +0x21A (set: its expression / lip update is skipped). The first log's phoneme sum stayed 0
// for every NPC, so every keyframe block of the face is read now and the moving ones are named. Smooth Lip Sync NG
// hooks the same face update (BSFaceGenNiNode::UpdateDownwardPass, 26985); its own log ('LIP START fg=...') shows the
// same face address as our 'face' value.

#include "LipLog.h"

#include "LF.h"

#include <intrin.h>

namespace LipLog
{
    namespace
    {
        using StartFn = float (*)(RE::Actor*, const char*, RE::BSSoundHandle*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        using PreloadFn = std::uint64_t (*)(RE::Actor*, const char*, std::uint64_t);
        using StopFn = void (*)(RE::AIProcess*, RE::Actor*);

        StartFn   g_start = nullptr;
        PreloadFn g_preload = nullptr;
        StopFn    g_stop = nullptr;

        constexpr float kNear = 2000.0f;  // other actors logged within this distance of the player (vanilla control)

        void Out(const std::string& a_text) { logger::info("Lip: {}", a_text); }

        // Short hex of a pointer for the log (enough to see that it changed).
        std::string Ptr(const void* a_p) { return a_p ? std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_p) & 0xFFFFFFF) : "none"; }

        // (146c) every follower ref with a preset (slot actors and taken-over NPCs), refreshed once a second by Loop
        std::mutex                     g_refLock;
        std::unordered_set<RE::FormID> g_lfRefs;

        // A Lazy Followers follower: base NPC from LazyFollowers.esp, or a follower ref with a preset (taken-over NPCs).
        bool Ours(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return false;
            }
            {
                std::lock_guard lock(g_refLock);
                if (g_lfRefs.contains(a_actor->GetFormID())) {
                    return true;
                }
            }
            auto base = a_actor->GetActorBase();
            auto file = base ? base->GetFile(0) : nullptr;
            return file && _stricmp(std::string(file->GetFilename()).c_str(), "LazyFollowers.esp") == 0;
        }

        // Logged: our followers, and other actors near the player.
        bool Watched(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return false;
            }
            if (Ours(a_actor)) {
                return true;
            }
            auto player = RE::PlayerCharacter::GetSingleton();
            return player && a_actor != player && a_actor->GetPosition().GetDistance(player->GetPosition()) < kNear;
        }

        std::string Name(RE::Actor* a_actor)
        {
            const char* n = a_actor->GetDisplayFullName();
            return std::format("{} {:08X}{}", n && *n ? n : "?", a_actor->GetFormID(), Ours(a_actor) ? " [LF]" : " [other]");
        }

        RE::HighProcessData* High(RE::Actor* a_actor)
        {
            auto proc = a_actor ? a_actor->GetActorRuntimeData().currentProcess : nullptr;
            return proc ? proc->high : nullptr;
        }

        // The actor's lip entry: "none" / "loading" / "loaded" / "failed" (+ entry address).
        std::string LipText(RE::Actor* a_actor)
        {
            auto high = High(a_actor);
            if (!high) {
                return "no high process";
            }
            auto entry = static_cast<const std::uint8_t*>(high->unk3B8);
            if (!entry) {
                return "none";
            }
            const auto  state = (*reinterpret_cast<const std::uint32_t*>(entry + 0x0C) >> 28) & 7;
            const char* what = state == 1 || state == 2 ? "loading" : state == 3 || state == 4 ? "loaded" : state == 0 || state == 5 ? "failed" : "?";
            return std::format("{} (state {}, entry {})", what, state, Ptr(entry));
        }

        float LipTimer(RE::Actor* a_actor)
        {
            auto high = High(a_actor);
            return high ? *reinterpret_cast<const float*>(&high->unk3C0) : 0.0f;
        }

        // A lip entry's state as text (same bits as the process lip: entry+0x0C bits 28-30).
        std::string EntryText(const std::uint8_t* a_entry)
        {
            if (!a_entry) {
                return "none";
            }
            const auto  state = (*reinterpret_cast<const std::uint32_t*>(a_entry + 0x0C) >> 28) & 7;
            const char* what = state == 1 || state == 2 ? "loading" : state == 3 || state == 4 ? "loaded" : state == 0 || state == 5 ? "failed" : "?";
            return std::format("{} ({})", what, Ptr(a_entry));
        }

        // (146c) The face: its own lip entry (+0x228), freeze flag (+0x21A), flag +0x215, and every keyframe block
        // (+0x20 .. +0x180, 0x20 apart; values at +0x10, count at +0x18) with a value - named by offset with the sum of
        // its values ("80:0.42" = phoneme block). a_moving: a block other than expression / modifier (+0x20 / +0x60) has a value.
        std::string FaceText(RE::Actor* a_actor, bool* a_moving = nullptr)
        {
            auto face = reinterpret_cast<const std::uint8_t*>(a_actor->GetFaceGenAnimationData());
            if (a_moving) {
                *a_moving = false;
            }
            if (!face) {
                return "face none";
            }
            std::string blocks;
            for (std::uint32_t off = 0x20; off <= 0x180; off += 0x20) {
                auto values = *reinterpret_cast<const float* const*>(face + off + 0x10);
                auto count = *reinterpret_cast<const std::uint32_t*>(face + off + 0x18);
                if (!values || count == 0 || count > 64) {
                    continue;
                }
                float sum = 0.0f;
                for (std::uint32_t i = 0; i < count; ++i) {
                    sum += std::abs(values[i]);
                }
                if (sum > 0.01f) {
                    blocks += std::format(" {:X}:{:.2f}", off, sum);
                    if (a_moving && off != 0x20 && off != 0x60) {
                        *a_moving = true;
                    }
                }
            }
            return std::format("face {} lip {} frozen {} f215 {} | blocks{}", Ptr(face), EntryText(*reinterpret_cast<const std::uint8_t* const*>(face + 0x228)),
                face[0x21A], face[0x215], blocks.empty() ? " none" : blocks);
        }

        // The game function a hooked call came from (by its return address), for the log.
        std::string Caller(std::uintptr_t a_ret)
        {
            static const std::vector<std::pair<std::uintptr_t, const char*>> known = [] {
                const std::pair<std::uint64_t, const char*> ids[] = { { 39772, "dialogue line (39772)" }, { 39771, "sound-descriptor line (39771)" },
                    { 37541, "Actor::UpdateInDialogue (37541)" }, { 37542, "line start (37542)" }, { 37544, "Actor::UpdateSoundCallBack (37544)" },
                    { 22864, "debug speak (22864)" }, { 44673, "44673" }, { 56220, "Papyrus Say (56220)" }, { 22202, "22202" }, { 22203, "22203" },
                    { 29054, "29054" }, { 37552, "37552" }, { 37741, "37741" } };
                std::vector<std::pair<std::uintptr_t, const char*>> out;
                for (auto& [id, name] : ids) {
                    out.emplace_back(REL::ID(id).address(), name);
                }
                return out;
            }();
            const char*    best = nullptr;
            std::uintptr_t bestAt = 0;
            for (auto& [at, name] : known) {
                if (a_ret > at && a_ret - at < 0x2000 && at > bestAt) {
                    best = name;
                    bestAt = at;
                }
            }
            return best ? std::string(best) : std::format("RVA {:X}", a_ret - REL::Module::get().base());
        }

        // ---- actors talking (watched until their sounds stopped and the lip is gone) ----
        struct Talk
        {
            RE::ActorHandle                       handle;
            std::string                           name;
            std::vector<std::uint32_t>            sounds;  // every sound ID a line start gave it during this talk
            std::chrono::steady_clock::time_point start;
            std::chrono::steady_clock::time_point lastStart;
            std::chrono::steady_clock::time_point lastPrint;
            std::string                           lastKey;
        };
        std::mutex                              g_lock;
        std::unordered_map<RE::FormID, Talk>    g_talks;
        // A line start that returned without a sound (lip still loading, too far...): the game's next tries are counted.
        struct Wait
        {
            std::string                           path;
            int                                   count = 0;
            std::chrono::steady_clock::time_point last;
        };
        std::unordered_map<RE::FormID, Wait> g_waits;

        void AddTalk(RE::Actor* a_actor, std::uint32_t a_sound)
        {
            const auto      now = std::chrono::steady_clock::now();
            std::lock_guard lock(g_lock);
            auto [it, fresh] = g_talks.try_emplace(a_actor->GetFormID());
            auto& t = it->second;
            if (fresh) {
                t.handle = a_actor->GetHandle();
                t.name = Name(a_actor);
                t.start = now;
            }
            t.lastStart = now;
            if (std::find(t.sounds.begin(), t.sounds.end(), a_sound) == t.sounds.end()) {
                t.sounds.push_back(a_sound);
            }
        }

        // ---- hooks (any thread the game calls them on; the game function always runs unchanged) ----
        float StartHook(RE::Actor* a_actor, const char* a_path, RE::BSSoundHandle* a_handle, std::uint64_t a4, std::uint64_t a5, std::uint64_t a6,
            std::uint64_t a7, std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11, std::uint64_t a12, std::uint64_t a13,
            std::uint64_t a14)
        {
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            bool       log = false;
            std::string lipBefore;
            std::uint32_t soundBefore = RE::BSSoundHandle::kInvalidID;
            try {
                log = Watched(a_actor);
                if (log) {
                    lipBefore = LipText(a_actor);
                    soundBefore = a_handle ? a_handle->soundID : RE::BSSoundHandle::kInvalidID;
                }
            } catch (...) {
                log = false;
            }
            const float length = g_start(a_actor, a_path, a_handle, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            if (!log) {
                return length;
            }
            try {
                const std::uint32_t sound = a_handle ? a_handle->soundID : RE::BSSoundHandle::kInvalidID;
                const bool          started = sound != RE::BSSoundHandle::kInvalidID && sound != soundBefore;
                const std::string   path = a_path ? a_path : "";
                const auto          id = a_actor->GetFormID();
                const auto now = std::chrono::steady_clock::now();
                if (!started) {
                    // the game tries again on later frames while the lip loads: one line for the first try only
                    // (the same line again within 10 s of the last try)
                    std::lock_guard lock(g_lock);
                    auto&           w = g_waits[id];
                    if (w.path == path && now - w.last < 10s) {
                        ++w.count;
                        w.last = now;
                        return length;
                    }
                    w = { path, 1, now };
                }
                int waited = 0;
                if (started) {
                    std::lock_guard lock(g_lock);
                    if (auto it = g_waits.find(id); it != g_waits.end()) {
                        waited = it->second.path == path ? it->second.count : 0;
                        g_waits.erase(it);
                    }
                }
                Out(std::format("{} - line start from {}: '{}' | background lip {} | lip before: {}, after: {} | {} | length {:.2f} s{} | 3D {} | {}",
                    Name(a_actor), Caller(ret), path, (a12 & 0xFF) != 0, lipBefore, LipText(a_actor),
                    started ? std::format("sound {:X} started{}", sound, a_handle == (High(a_actor) ? &High(a_actor)->soundHandles[0] : nullptr) ? " (soundHandles[0])" : " (other handle)") :
                              std::string("NO sound started (waits for the lip / too far / no 3D)"),
                    length, waited ? std::format(" | after {} waiting tries", waited) : "", Ptr(a_actor->Get3D(false)), FaceText(a_actor)));
                if (started) {
                    AddTalk(a_actor, sound);
                }
            } catch (...) {
            }
            return length;
        }

        std::uint64_t PreloadHook(RE::Actor* a_actor, const char* a_path, std::uint64_t a_background)
        {
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            bool       log = false;
            try {
                log = Watched(a_actor);
            } catch (...) {
            }
            const auto result = g_preload(a_actor, a_path, a_background);
            if (log) {
                try {
                    Out(std::format("{} - lip request from {}: '{}' | background {} | lip now: {}", Name(a_actor), Caller(ret), a_path ? a_path : "",
                        (a_background & 0xFF) != 0, LipText(a_actor)));
                } catch (...) {
                }
            }
            return result;
        }

        void StopHook(RE::AIProcess* a_process, RE::Actor* a_actor)
        {
            const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
            try {
                // only when there is a lip to stop (the game calls this a lot)
                if (a_process && a_process->high && a_process->high->unk3B8 && Watched(a_actor)) {
                    Out(std::format("{} - lip stopped by {} | lip was: {}, timer {:.2f} s", Name(a_actor), Caller(ret), LipText(a_actor), LipTimer(a_actor)));
                }
            } catch (...) {
            }
            g_stop(a_process, a_actor);
        }

        // ---- sampler: main thread, 10 times a second while someone we watch talks ----
        std::atomic<bool> g_samplePending{ false };
        std::atomic<long long> g_lastSampleMs{ 0 };  // steady-clock ms when the main thread last ran a sample

        long long NowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void Sample()
        {
            g_lastSampleMs = NowMs();
            g_samplePending = false;
            const auto      now = std::chrono::steady_clock::now();
            std::lock_guard lock(g_lock);
            for (auto it = g_talks.begin(); it != g_talks.end();) {
                auto& t = it->second;
                auto  ptr = t.handle.get();
                auto  a = ptr.get();
                const float talked = std::chrono::duration<float>(now - t.start).count();
                if (!a) {
                    Out(std::format("{} - gone (no actor) after {:.1f} s", t.name, talked));
                    it = g_talks.erase(it);
                    continue;
                }
                std::string sounds;
                bool        anyPlaying = false;
                for (auto id : t.sounds) {
                    RE::BSSoundHandle h;
                    h.soundID = id;
                    const bool on = h.IsPlaying();
                    anyPlaying |= on;
                    sounds += std::format("{}{:X} {}", sounds.empty() ? "" : ", ", id, on ? "playing" : "stopped");
                }
                auto        high = High(a);
                std::string handle0 = high ? std::format("{:X}", high->soundHandles[0].soundID) : "-";
                std::string item = "none";
                if (high && high->greetTopic) {
                    auto info = high->greetTopic->info;
                    item = info ? std::format("{:08X}", info->GetFormID()) : "no info";
                }
                bool       moving = false;
                const auto face = FaceText(a, &moving);
                const auto lip = LipText(a);
                auto       faceEntry = a->GetFaceGenAnimationData() ? *reinterpret_cast<const std::uint8_t* const*>(reinterpret_cast<const std::uint8_t*>(a->GetFaceGenAnimationData()) + 0x228) : nullptr;
                const auto key = std::format("{}|{}|{}|{}|{}|{}|{}|{}", sounds, handle0, lip, moving, Ptr(a->Get3D(false)), Ptr(a->GetFaceGenAnimationData()), EntryText(faceEntry), item);
                const bool lipGone = !high || !high->unk3B8;
                if (key != t.lastKey || now - t.lastPrint >= 1s) {
                    Out(std::format("{} - {:.1f} s: sounds [{}] | soundHandles[0] {} | process lip {} | 3D {} | {} (mouth blocks {}) | dialogue item {}",
                        t.name, talked, sounds, handle0, lip, Ptr(a->Get3D(false)), face, moving ? "moving" : "still", item));
                    t.lastKey = key;
                    t.lastPrint = now;
                }
                if ((!anyPlaying && lipGone && now - t.lastStart > 1500ms) || talked > 60.0f) {
                    Out(std::format("{} - talk over after {:.1f} s ({} sound(s))", t.name, talked, t.sounds.size()));
                    it = g_talks.erase(it);
                    continue;
                }
                ++it;
            }
        }

        // Skyrim is the foreground window (same test as Hotkey.cpp).
        bool GameHasFocus()
        {
            HWND fg = GetForegroundWindow();
            if (!fg) {
                return false;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(fg, &pid);
            return pid == GetCurrentProcessId();
        }

        // Own thread: focus changes + a stalled main thread are logged in real time; a sample is queued every 100 ms
        // while someone is talking.
        void Loop()
        {
            bool focus = GameHasFocus();
            bool stallLogged = false;
            for (int tick = 0;; ++tick) {
                std::this_thread::sleep_for(100ms);
                if (tick % 10 == 0) {
                    // (146c) taken-over NPCs count as ours ([LF])
                    std::unordered_set<RE::FormID> refs;
                    LF::ForEachFollowerPreset([&refs](RE::FormID a_ref, const std::string&, const std::string&) { refs.insert(a_ref); });
                    std::lock_guard lock(g_refLock);
                    g_lfRefs = std::move(refs);
                }
                const bool f = GameHasFocus();
                if (f != focus) {
                    focus = f;
                    std::lock_guard lock(g_lock);
                    Out(std::format("game window {} ({} actor(s) talking)", f ? "has focus again" : "lost focus", g_talks.size()));
                }
                bool talking;
                {
                    std::lock_guard lock(g_lock);
                    talking = !g_talks.empty();
                }
                if (!talking) {
                    continue;
                }
                if (g_samplePending) {
                    const auto since = NowMs() - g_lastSampleMs;
                    if (since > 1000 && !stallLogged) {
                        Out(std::format("main thread did not run for {:.1f} s (game paused?)", since / 1000.0));
                        stallLogged = true;
                    }
                    continue;
                }
                if (stallLogged) {
                    Out("main thread runs again");
                    stallLogged = false;
                }
                g_samplePending = true;
                if (g_lastSampleMs == 0) {
                    g_lastSampleMs = NowMs();
                }
                SKSE::GetTaskInterface()->AddTask([]() {
                    try {
                        Sample();
                    } catch (...) {
                        g_samplePending = false;
                    }
                });
            }
        }

        // Every direct call (E8) of a_target in the game's code.
        std::vector<std::uintptr_t> CallsTo(std::uintptr_t a_target)
        {
            const auto  text = REL::Module::get().segment(REL::Segment::textx);
            const auto  start = text.address();
            const auto* bytes = text.pointer<const std::uint8_t>();
            std::vector<std::uintptr_t> out;
            for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
                if (bytes[i] != 0xE8) {
                    continue;
                }
                std::int32_t rel;
                std::memcpy(&rel, bytes + i + 1, sizeof(rel));
                if (static_cast<std::uintptr_t>(static_cast<std::intptr_t>(start + i + 5) + rel) == a_target) {
                    out.push_back(start + i);
                }
            }
            return out;
        }
    }

    void Install()
    {
        static bool done = false;
        if (done) {
            return;
        }
        done = true;
        if (REL::Module::get().version() != REL::Version(1, 6, 1170, 0)) {
            logger::info("Lip: game is not 1.6.1170 - lip sync log off");
            return;
        }
        const auto start = REL::ID(37542).address();
        const auto preload = REL::ID(37545).address();
        const auto stop = REL::ID(39376).address();
        const auto startSites = CallsTo(start);
        const auto preloadSites = CallsTo(preload);
        const auto stopSites = CallsTo(stop);
        g_start = reinterpret_cast<StartFn>(start);
        g_preload = reinterpret_cast<PreloadFn>(preload);
        g_stop = reinterpret_cast<StopFn>(stop);
        SKSE::AllocTrampoline((startSites.size() + preloadSites.size() + stopSites.size()) * 14);
        auto& tr = SKSE::GetTrampoline();
        for (auto at : startSites) {
            tr.write_call<5>(at, StartHook);
        }
        for (auto at : preloadSites) {
            tr.write_call<5>(at, PreloadHook);
        }
        for (auto at : stopSites) {
            tr.write_call<5>(at, StopHook);
        }
        logger::info("Lip: lip sync log on - hooked {} line start calls (RE: 4), {} lip request calls (RE: 1), {} lip stop calls (RE: 11)",
            startSites.size(), preloadSites.size(), stopSites.size());
        std::thread(Loop).detach();
    }
}
