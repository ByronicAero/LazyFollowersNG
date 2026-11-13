// LF - Lazy Followers' logic in C++. Replaces the old Papyrus scripts:
//   _LazyFollowersActorScript  -> Slot state, ApplyBase / RefreshAppearance / ScheduleUpdate, Dismiss, Summon
//   _LazyFollowersPlayerScript -> OnGameLoaded (refresh every follower after a load)
//   _LazyFollowersMCMScript    -> the actions behind the SMF pages (Menu.cpp)
//   _LazyFollowersConfigManager -> Save / Load Config and NPC files (with Files.cpp)
// The scripts' per-follower variables live in the SKSE co-save ('LFNG'). Things the game saves by itself
// (enabled / position, race, added spells, inventory, actor values) are not duplicated. Base-form edits
// (name, voice type, combat style, class, combat style values) are not saved by the game, so they are
// applied again on every load - like the scripts did in OnLoad / OnPlayerLoadGame.
//
// Fixes compared to the old mod:
// - Race list: built from the load order every time a game loads and the Followers page opens (the old
//   list lived in the save and its names / forms went out of step, so new races never showed up).
// - Save Actor Preset: RaceMenu's "skee preset-save" console command reads the console target without
//   checking it exists (null -> crash; "preset-load" does check). We always hand RaceMenu a real, loaded
//   actor through CharGen.SaveCharacterPreset, so that crash cannot happen.
// - Config load, NPC file load (sex check key, gaps in the spell list) and "Save all NPCs to one file"
//   (wrote separate files) work as their buttons say.
// - NG 1.1: any slot can be male or female (the base NPC's sex follows the applied preset, kept in the
//   co-save and set again on every load), 100 slots (esp), Nearby Actors for Save Actor Preset.

#include "LF.h"

#include "EslIds.h"
#include "Files.h"
#include "Papyrus.h"
#include "Tint.h"
#include "Voice.h"

// CommonLibSSE-NG 3.7 only declares TESPackageEvent (the event behind Papyrus OnPackageStart); layout as
// in powerof3's CommonLibSSE. We only read the actor and the type.
namespace RE
{
    struct TESPackageEvent
    {
        NiPointer<TESObjectREFR> actor;    // 00
        FormID                   package;  // 08
        std::uint32_t            type;     // 0C: 0 start, 1 change, 2 end
    };
    static_assert(sizeof(TESPackageEvent) == 0x10);
}

namespace LF
{
    namespace
    {
        constexpr std::string_view kPlugin = "LazyFollowers.esp";

        // LazyFollowers.esp local form IDs. The esp is ESL-flagged (Tools/eslify.py renumbered its forms to
        // 0x800+); written here as the 0.42.3 IDs (Tools/esp.py on the old esp) and mapped with EslIds.h.
        constexpr RE::FormID kNPCsList = EslId(0x014F62);
        constexpr RE::FormID kStylesList = EslId(0x014F5F);
        constexpr RE::FormID kFemaleVoicesList = EslId(0x014F5D);
        constexpr RE::FormID kMaleVoicesList = EslId(0x014F5E);
        constexpr RE::FormID kSpellsList = EslId(0x033588);
        constexpr RE::FormID kClassesList = EslId(0x0479B5);
        constexpr RE::FormID kFaction = EslId(0x014F73);
        constexpr RE::FormID kCellMarker = EslId(0x014F61);  // REFR of _LazyFollowersCellMarker in the storage cell
        constexpr RE::FormID kRacesList = EslId(0x014F72);
        constexpr std::array<RE::FormID, static_cast<int>(Global::kCount)> kGlobalIDs{ EslId(0x060EF4), EslId(0x060EF2), EslId(0x060EF3), EslId(0x060EF5),
            EslId(0x0701F6) };
        static_assert(kNPCsList && kStylesList && kFemaleVoicesList && kMaleVoicesList && kSpellsList && kClassesList && kFaction && kCellMarker && kRacesList &&
                      kGlobalIDs[0] && kGlobalIDs[1] && kGlobalIDs[2] && kGlobalIDs[3] && kGlobalIDs[4]);
        constexpr std::array<int, static_cast<int>(Global::kCount)>        kGlobalDefaults{ 1, 0, 11, 1, 1 };  // values in the esp

        // Names the MCM showed for its form lists (its VMAD string arrays, same order as the lists).
        constexpr std::array<const char*, 18> kFemaleVoiceNames{ "EvenToned*", "Nord", "Commoner", "Commander", "Condescending*", "ElfHaughty", "DarkElf",
            "YoungEager*", "Sultry*", "Orc", "Khajiit", "Shrill", "Coward", "OldGrumpy", "OldKindly", "Argonian", "DarkElfCommoner", "Vampire" };
        constexpr std::array<const char*, 26> kMaleVoiceNames{ "EvenToned*", "Nord*", "EvenTonedAccented*", "YoungEager*", "ElfHaughty", "Commoner",
            "Commander", "DarkElf*", "Orc*", "CommonerAccented", "Brute*", "Drunk*", "Khajiit*", "SlyCynical", "Soldier", "Condescending", "Argonian*",
            "NordCommander", "Warlock", "OldGrumpy", "Coward", "OldKindly", "DarkElfCommoner", "bandit", "DarkElfCynical", "Guard" };
        constexpr std::array<const char*, 19> kClassNames{ "LazyClass", "Assassin", "Barbarian", "MageConjurer", "MageDestruction", "MageElemental",
            "MageNecro", "Monk", "Mystic", "NightBlade", "NightingGale", "Ranger", "Rogue", "Scout", "Sorcerer", "Spellsword", "Thief", "Warrior1H",
            "Warrior2H" };
        constexpr std::array<const char*, kStyles> kStyleDefaultNames{ "1H (spell)Sword", "Magic Archer", "1H tank", "Caster", "CombatStyle #4",
            "CombatStyle #5", "CombatStyle #6", "CombatStyle #7", "CombatStyle #8", "CombatStyle #9" };

        // Stats page values (Papyrus actor value names, for ForceActorValue) and skills.
        constexpr std::array<std::pair<const char*, RE::ActorValue>, kStatCount> kStats{ {
            { "WeaponSpeedMult", RE::ActorValue::kWeaponSpeedMult },
            { "BowSpeedBonus", RE::ActorValue::kBowSpeedBonus },
            { "AttackDamageMult", RE::ActorValue::kAttackDamageMult },
            { "SpeedMult", RE::ActorValue::kSpeedMult },
            { "DamageResist", RE::ActorValue::kDamageResist },
            { "MagicResist", RE::ActorValue::kResistMagic },
            { "Health", RE::ActorValue::kHealth },
            { "Stamina", RE::ActorValue::kStamina },
            { "Magicka", RE::ActorValue::kMagicka },
            { "HealRate", RE::ActorValue::kHealRate },
            { "StaminaRate", RE::ActorValue::kStaminaRate },
            { "MagickaRate", RE::ActorValue::kMagickaRate },
        } };
        constexpr std::array<std::pair<const char*, RE::ActorValue>, kSkillCount> kSkills{ {
            { "OneHanded", RE::ActorValue::kOneHanded },
            { "TwoHanded", RE::ActorValue::kTwoHanded },
            { "Marksman", RE::ActorValue::kArchery },
            { "Block", RE::ActorValue::kBlock },
            { "Smithing", RE::ActorValue::kSmithing },
            { "HeavyArmor", RE::ActorValue::kHeavyArmor },
            { "LightArmor", RE::ActorValue::kLightArmor },
            { "Pickpocket", RE::ActorValue::kPickpocket },
            { "Lockpicking", RE::ActorValue::kLockpicking },
            { "Sneak", RE::ActorValue::kSneak },
            { "Alchemy", RE::ActorValue::kAlchemy },
            { "Speechcraft", RE::ActorValue::kSpeech },
            { "Alteration", RE::ActorValue::kAlteration },
            { "Conjuration", RE::ActorValue::kConjuration },
            { "Destruction", RE::ActorValue::kDestruction },
            { "Illusion", RE::ActorValue::kIllusion },
            { "Restoration", RE::ActorValue::kRestoration },
            { "Enchanting", RE::ActorValue::kEnchanting },
        } };

        // ---- forms (kDataLoaded) ----
        bool                                    g_formsFound = false;
        std::vector<RE::Actor*>                 g_actors;  // the slots' actors (persistent references); nullptr for real NPC entries
        int                                     g_lfCount = 0;  // NG 1.2: g_slots = the esp's slots, then kRealSlots real NPC entries
        // main thread only (NG perf): Apply to All for a follower whose 3D is not loaded = its full apply when it
        // loads.
        std::vector<std::uint8_t> g_fullOnLoad;
        std::array<RE::TESGlobal*, static_cast<int>(Global::kCount)> g_globals{};
        std::array<RE::TESCombatStyle*, kStyles> g_styles{};
        std::array<std::array<float, kStyleValues>, kStyles> g_styleDefaults{};
        RE::BGSListForm*  g_femaleVoicesList = nullptr;
        RE::BGSListForm*  g_maleVoicesList = nullptr;
        RE::BGSListForm*  g_spellsList = nullptr;
        RE::BGSListForm*  g_classesList = nullptr;
        RE::TESFaction*   g_faction = nullptr;
        RE::TESFaction*   g_followerFaction = nullptr;  // Skyrim CurrentFollowerFaction
        RE::TESObjectREFR* g_marker = nullptr;
        RE::BGSColorForm* g_hairColor = nullptr;        // RaceMenu.esp 801 (the scripts' rmHairColor)
        RE::TESQuest*     g_fgtQuest = nullptr;         // Follower Goes on a Trip's FGTDialogueQuest

        // ---- state (main thread writes; the menu copies it under the lock) ----
        std::recursive_mutex g_lock;
        std::vector<Slot>    g_slots;
        std::array<std::string, kStyles> g_styleNames;
        std::vector<Choice>  g_extraFemaleVoices;  // voices met (crosshair NPCs / config), saved in the co-save
        std::vector<Choice>  g_extraMaleVoices;
        std::vector<RE::FormID> g_extraRaces;      // non-playable races met (crosshair NPCs / config)
        bool g_stylesEnabled = true;               // MCM property _LazyFollowersCombatStyleEnabled (esp: true)
        bool g_classesEnabled = true;              // _LazyFollowersClassEnabled (esp: true)
        bool g_autoEquip = false;
        bool g_includeEquip = true;  // NG (user): Copy Kit also copies the armor / weapons you wear (off: only the rest)
        std::atomic<bool> g_keepPending{ false };  // a new game waits for RaceMenu to close, then gets the kept followers
        std::unordered_set<int> g_stayPut;  // main thread: real NPC entries whose next full apply must not bring them to the player
        std::unordered_set<RE::FormID> g_restoring;  // main thread: real NPCs whose give-back (RestoreRealLook) still runs
        bool g_allSpells = false;
        bool g_spellsEnabled = true;               // new (Settings): off greys out the Spells page
        float g_scanRadius = kDefaultScanRadius;   // Nearby Actors scan radius (metres)
        int   g_nearbyRows = kDefaultNearbyRows;   // Nearby Actors table rows before it scrolls
        std::vector<float>         g_nearbyWidths;  // Nearby Actors column widths (empty = default)
        int                        g_presetRows = kDefaultNearbyRows;  // NG (user): Settings > Presets table rows before it scrolls
        std::vector<float>         g_presetWidths;  // ... and its column widths (empty = default)
        std::atomic<std::uint64_t> g_openKey{ 0 };  // Open Window hotkey: vk | mods << 32 (read by the hotkey thread)
        std::atomic<std::uint64_t> g_summonKey{ 0 };  // Summon hotkey, same layout
        std::atomic<std::uint64_t> g_summonAllKey{ 0 };  // Summon All hotkey, same layout
        std::atomic<std::uint64_t> g_savePresetKey{ 0 };  // Save Player Preset hotkey, same layout
        std::atomic<std::uint64_t> g_openInvKey{ 0 };     // NG (user): Open Inventory hotkey, same layout
        std::atomic<std::uint64_t> g_nearbyWinKey{ 0 };   // NG (user): Nearby Actors Window hotkey, same layout
        std::shared_ptr<const Lists> g_lists = std::make_shared<Lists>();

        std::atomic<bool>     g_inGame{ false };
        std::atomic<unsigned> g_generation{ 0 };  // bumped on every load / revert: old delayed jobs stop
        std::atomic<int>      g_selectRequest{ -1 };
        std::atomic<bool>     g_selectMoved{ false };  // the request comes from a take-over / give-back (Apply)

        std::mutex          g_crossLock;
        RE::ObjectRefHandle g_crosshair;       // under the crosshair now (none when it leaves)
        RE::ObjectRefHandle g_lastCrossActor;  // NG (user): the last actor (not you) under the crosshair - kept when it leaves

        std::mutex        g_snapLock;
        std::shared_ptr<const Snapshot> g_snap = std::make_shared<Snapshot>();
        std::atomic<bool> g_snapQueued{ false };
        std::atomic<int>  g_snapSelected{ 0 };

        std::mutex               g_nearbyLock;
        std::vector<NearbyActor> g_nearby;
        std::atomic<bool>        g_scanning{ false };

        std::mutex  g_noticeLock;
        std::string g_notice;
        bool        g_noticeError = false;

        // ---- small helpers ----

        template <class T = RE::TESForm>
        T* Lookup(RE::FormID a_id)
        {
            return a_id ? RE::TESForm::LookupByID<T>(a_id) : nullptr;
        }

        RE::FormID IdOf(const RE::TESForm* a_form) { return a_form ? a_form->GetFormID() : 0; }

        // A slot's preset is the bare name ("Ciel", the MCM's parsePreset): RaceMenu's CharGen.Load*Ex add
        // "SKSE\Plugins\CharGen\Exported\<name>.jslot" and "Textures\CharGen\Exported\<name>.dds" themselves.
        // Build 1.0.0 stored "../Exported/<name>", which broke the tint path (face lost its textures); strip it.
        std::string PresetKey(std::string a_preset)
        {
            constexpr std::string_view kOld = "../Exported/";
            if (a_preset.starts_with(kOld)) {
                a_preset.erase(0, kOld.size());
            }
            return a_preset;
        }

        // Log line (the scripts' dout). NG (user): Logging is an on / off toggle now - the old "Console" mode
        // (console printing) is gone; off = only warnings and errors reach the log (ApplyLogLevel).
        void Out(const std::string& a_text) { logger::info("{}", a_text); }

        // The log level follows the Logging toggle (global _LazyFollowersTraceMode): 2 (off) = warnings and errors
        // only, else everything.
        void ApplyLogLevel()
        {
            const auto g = g_globals[static_cast<int>(Global::kTrace)];
            const bool errorsOnly = g && g->value > 1.5f;
            if (auto log = spdlog::default_logger()) {
                log->set_level(errorsOnly ? spdlog::level::warn : spdlog::level::info);
            }
        }

        // Every form in a FormList (esp entries + ones scripts added), in GetAt order.
        std::vector<RE::TESForm*> ListForms(RE::BGSListForm* a_list)
        {
            std::vector<RE::TESForm*> out;
            if (!a_list) {
                return out;
            }
            for (auto f : a_list->forms) {
                out.push_back(f);
            }
            if (a_list->scriptAddedTempForms) {
                for (auto id : *a_list->scriptAddedTempForms) {
                    out.push_back(RE::TESForm::LookupByID(id));
                }
            }
            return out;
        }

        // The slot's actor. NG 1.2: a real NPC entry's actor is looked up by its reference each time (it can be
        // unloaded); nullptr when it is not in the game right now or the entry is free.
        RE::Actor* ActorOf(int a_slot)
        {
            if (a_slot < 0 || a_slot >= static_cast<int>(g_actors.size())) {
                return nullptr;
            }
            if (g_actors[a_slot]) {
                return g_actors[a_slot];
            }
            RE::FormID ref = 0;
            {
                std::lock_guard lock(g_lock);
                if (a_slot < static_cast<int>(g_slots.size()) && g_slots[a_slot].real) {
                    ref = g_slots[a_slot].ref;
                }
            }
            return ref ? RE::TESForm::LookupByID<RE::Actor>(ref) : nullptr;
        }

        // A slot's default name (a real NPC: its own name).
        std::string DefaultName(int a_slot)
        {
            std::lock_guard lock(g_lock);
            return a_slot >= 0 && a_slot < static_cast<int>(g_slots.size()) ? g_slots[a_slot].defaultName : std::string{};
        }

        // NG 1.2: a_slot is a real NPC entry (Slot::real).
        bool IsReal(int a_slot)
        {
            std::lock_guard lock(g_lock);
            return a_slot >= 0 && a_slot < static_cast<int>(g_slots.size()) && g_slots[a_slot].real;
        }

        // The last actor you looked at (Nearby Actors picks it up by itself).
        RE::Actor* CrosshairActor()
        {
            std::lock_guard lock(g_crossLock);
            auto            ref = g_lastCrossActor.get();
            return ref ? ref->As<RE::Actor>() : nullptr;
        }

        std::atomic<RE::FormID> g_lastConsoleActor{ 0 };  // NG: the last actor (not you) clicked in the console

        // The console's target actor (nullptr: nothing selected in the console). NG fix (user video 08-19-05: Vicky
        // clicked in the console, then a click on her hit the light beam in front of her - FXAmbBeamDust01 - and the
        // crosshair won again): while ANY reference is selected in the console it is the console's turn; a non-actor
        // selection keeps the last actor clicked there.
        RE::Actor* ConsoleActor()
        {
            auto ref = RE::Console::GetSelectedRef();
            if (!ref) {
                return nullptr;
            }
            if (auto actor = ref->As<RE::Actor>()) {
                if (!actor->IsPlayerRef()) {
                    g_lastConsoleActor = actor->GetFormID();
                    return actor;
                }
            }
            const auto last = g_lastConsoleActor.load();
            return last ? RE::TESForm::LookupByID<RE::Actor>(last) : nullptr;
        }

        // Something (actor or not) is selected in the console right now.
        bool ConsoleHasSelection() { return static_cast<bool>(RE::Console::GetSelectedRef()); }

        std::string DisplayName(RE::TESObjectREFR* a_ref)
        {
            if (!a_ref) {
                return {};
            }
            const char* n = a_ref->GetDisplayFullName();
            return n && *n ? n : std::format("{:08X}", a_ref->GetFormID());
        }

        // The slot (esp slot or real NPC entry) of a_actor, -1 = none. By reference ID (= a slot's ref).
        int SlotOfActor(const RE::Actor* a_actor)
        {
            if (!a_actor) {
                return -1;
            }
            const auto      id = a_actor->GetFormID();
            std::lock_guard lock(g_lock);
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                if (g_slots[i].ref == id) {
                    return i;
                }
            }
            return -1;
        }

        bool Following(RE::Actor* a_actor)
        {
            return a_actor && g_followerFaction && a_actor->GetFactionRank(g_followerFaction, false) >= 1;
        }

        // Runs a_fn on the main thread (SKSE task).
        void OnMain(std::function<void()> a_fn)
        {
            SKSE::GetTaskInterface()->AddTask([fn = std::move(a_fn)]() { fn(); });
        }

        // Delayed jobs (NG perf): one timer thread runs them in time order (was one new thread per job, many at
        // once when a save with several followers loads).
        std::mutex                                                               g_timerLock;
        std::condition_variable                                                  g_timerWake;
        std::multimap<std::chrono::steady_clock::time_point, std::function<void()>> g_timerJobs;

        void TimerLoop()
        {
            std::unique_lock lock(g_timerLock);
            for (;;) {
                if (g_timerJobs.empty()) {
                    g_timerWake.wait(lock);
                    continue;
                }
                const auto due = g_timerJobs.begin()->first;
                if (std::chrono::steady_clock::now() < due) {
                    g_timerWake.wait_until(lock, due);  // woken early by a sooner job, or due now
                    continue;
                }
                auto fn = std::move(g_timerJobs.begin()->second);
                g_timerJobs.erase(g_timerJobs.begin());
                lock.unlock();
                OnMain(std::move(fn));
                lock.lock();
            }
        }

        // Runs a_fn on the main thread after a_seconds, unless a game was loaded meanwhile.
        void After(float a_seconds, std::function<void()> a_fn)
        {
            static std::once_flag started;
            std::call_once(started, []() { std::thread(TimerLoop).detach(); });
            const unsigned gen = g_generation.load();
            const auto     due = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(std::max(0.0f, a_seconds) * 1000.0f));
            {
                std::lock_guard lock(g_timerLock);
                g_timerJobs.emplace(due, [gen, fn = std::move(a_fn)]() {
                    if (gen == g_generation.load() && g_inGame.load()) {
                        fn();
                    }
                });
            }
            g_timerWake.notify_one();
        }

        // ---- per-follower game changes (main thread) ----

        void ScheduleUpdate(int a_slot, bool a_full, bool a_playerFace = false, bool a_body = false);

        // NG fix (user crash log: skee64 SKSETaskApplyMorphNormals on a head still being built; "small head"):
        // RaceMenu rebuilds the head on a background thread after every SetRace / LoadCharacterEx, and our
        // passes (full apply, its lite pass, the summon's lite pass, the 3D-load pass) used to start within
        // ~50 ms of each other. Now one appearance pass per follower at a time, kSettle seconds after each
        // before the next; requests that come meanwhile are merged into one. Inside a pass a wait follows
        // every head rebuild (SetRace, then LoadCharacterEx before the body-only one).
        constexpr float kSettle = 1.0f;

        // A follower's appearance pass state (main thread only). gen = the load it belongs to (a load drops
        // timers, so a pass of an older load counts as finished).
        struct PassState
        {
            bool                                  running = false;
            unsigned                              gen = 0;
            std::chrono::steady_clock::time_point started{};
            bool                               pending = false;  // a request came while running: run it afterwards
            bool                               full = false;
            bool                               playerFace = false;
            bool                               body = false;
            std::vector<std::function<void()>> thens;
        };
        std::vector<PassState> g_passes;     // per slot
        std::vector<std::uint8_t> g_resetLook;  // per slot: next full pass first clears the preset (Apply Settings)
        std::vector<std::chrono::steady_clock::time_point> g_freshUntil;  // per slot: a full apply just finished on the loaded actor - lite passes until then skip the reload

        // A chain step that waits a_seconds (main-thread timer), then lets the chain go on.
        Papyrus::Chain::Step WaitStep(float a_seconds)
        {
            return [a_seconds](Papyrus::Done a_done) {
                After(a_seconds, [a_done]() { a_done(Papyrus::Variable{}); });
                return true;
            };
        }

        // Waits until a_actor's 3D is there (checked every 0.25 s, a_tries times at most), then reports to a_done.
        // Does not wait for an actor that is disabled or in a cell that is not loaded (storage).
        void WaitLoaded(RE::Actor* a_actor, int a_tries, Papyrus::Done a_done)
        {
            auto cell = a_actor->GetParentCell();
            if (a_tries <= 0 || a_actor->Is3DLoaded() || a_actor->IsDisabled() || !cell || !cell->IsAttached()) {
                a_done(Papyrus::Variable{});
                return;
            }
            After(0.25f, [a_actor, a_tries, a_done]() { WaitLoaded(a_actor, a_tries - 1, a_done); });
        }

        // NG (user): Mu Dynamic NPC Replacer adds its keyword MuDynamicNPCReplacer (MuDynamicNPCReplacer.esp
        // 0xFFE) to the base NPC it changed when its config lists that keyword (REA: ApplyActorData1 adds the
        // preset's keywords to TESNPC's keyword form). Such a follower's look is Mu Dynamic's: we leave its
        // race, RaceMenu preset and sex alone (our ClearPreset / SetRace / LoadCharacterEx wiped it).
        // NG 1.2 (user log 2026-11-11 10:35: Mu put its keyword on the real Sofia right after she got Ciel's preset -
        // its config matched her new look / name - and Remove then left her look alone): a real NPC entry is never
        // left to Mu.
        bool MuManaged(RE::Actor* a_actor)
        {
            if (const int slot = SlotOfActor(a_actor); slot >= 0 && IsReal(slot)) {
                return false;
            }
            static RE::BGSKeyword* const kw = []() -> RE::BGSKeyword* {
                auto dh = RE::TESDataHandler::GetSingleton();
                return dh && dh->LookupModByName("MuDynamicNPCReplacer.esp") ? dh->LookupForm<RE::BGSKeyword>(0xFFE, "MuDynamicNPCReplacer.esp") : nullptr;
            }();
            auto npc = a_actor ? a_actor->GetActorBase() : nullptr;
            return kw && npc && npc->HasKeyword(kw);
        }

        // NG (user crash 2026-11-12, Sofia + Ciel_Erin): Mu Dynamic NPC Replacer also changes a_owner's look - its
        // config targets the NPC (Files::MuReplacerConfig) or its keyword is already on the base. Two mods putting
        // presets on one NPC crashed the game, so a take-over is refused with a notice saying what to delete.
        // "" = no conflict.
        std::string MuConflict(RE::Actor* a_owner)
        {
            auto npc = a_owner ? a_owner->GetActorBase() : nullptr;
            auto file = npc ? npc->GetFile(0) : nullptr;
            if (!npc || !file) {
                return {};
            }
            const std::uint32_t local = file->IsLight() ? (npc->GetFormID() & 0xFFF) : (npc->GetFormID() & 0xFFFFFF);
            if (auto cfg = Files::MuReplacerConfig(file->fileName, local); !cfg.empty()) {
                return std::format("{} has a Mu Dynamic NPC Replacer config ({}) - delete it first (MO2: the Overwrite folder,\n"
                                   "SKSE/Plugins/MuDynamicNPCReplacer), then load the game again. Two mods changing one NPC's look crashes.",
                    DisplayName(a_owner), cfg);
            }
            static RE::BGSKeyword* const kw = []() -> RE::BGSKeyword* {
                auto dh = RE::TESDataHandler::GetSingleton();
                return dh && dh->LookupModByName("MuDynamicNPCReplacer.esp") ? dh->LookupForm<RE::BGSKeyword>(0xFFE, "MuDynamicNPCReplacer.esp") : nullptr;
            }();
            if (kw && npc->HasKeyword(kw)) {
                return std::format("{} is changed by Mu Dynamic NPC Replacer - remove it there (its config / preset) first,\n"
                                   "then load the game again. Two mods changing one NPC's look crashes.",
                    DisplayName(a_owner));
            }
            return {};
        }

        std::atomic<unsigned> g_playerFaceTicket{ 0 };

        // RaceMenu side effect (old mod too): after a preset is loaded on a follower the PLAYER's face can lose
        // its textures until "showracemenu". Same refresh here: the player's tint masks (SKSE
        // Game.UpdateTintMaskColors) + new head (Actor.RegenerateHead) + 3D update (RaceMenu re-applies overlays).
        // Several requests close together (Summon All) = one refresh 1 s after the last.
        void RefreshPlayerFace()
        {
            const unsigned ticket = ++g_playerFaceTicket;
            After(1.0f, [ticket]() {
                auto player = RE::PlayerCharacter::GetSingleton();
                if (ticket != g_playerFaceTicket.load() || !player) {
                    return;
                }
                auto chain = Papyrus::Chain::Make();
                // Test 1 (tint masks + 3D update only) did not fix it: also regenerate the head (SKSE
                // Actor.RegenerateHead = new FaceGen head + tint texture, what opening RaceMenu does).
                chain->Then([](Papyrus::Done d) { return Papyrus::CallStatic("Game", "UpdateTintMaskColors", d); });
                chain->Then([player](Papyrus::Done d) { return Papyrus::CallMethod(player, "Actor", "RegenerateHead", d); });
                chain->Then([player](Papyrus::Done d) { return Papyrus::CallMethod(player, "Actor", "QueueNiNodeUpdate", d); });
                chain->ThenDo([]() { logger::info("Player face refreshed (tint masks + head regenerated + 3D update)"); });
                chain->Run();
            });
        }

        // The slot's name on the base NPC + the reference's display name (the scripts' checkmyPresetName).
        // The game drops it again (SetRace / packages - the old script re-set it in OnPackageStart), so this
        // also runs after every preset apply and when a follower's package starts.
        // a_ref = false: base NPC only (while a save is being reverted the references are not ready).
        void ApplyName(int a_slot, bool a_ref = true)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                return;
            }
            std::string name;
            {
                std::lock_guard lock(g_lock);
                const auto&     s = g_slots[a_slot];
                if (s.real && !s.Active()) {
                    return;  // NG 1.2: a real NPC without preset keeps its own name (Remove restores it, RestoreRealBase)
                }
                name = s.Active() && !s.name.empty() ? s.name : s.defaultName;
            }
            if (name != npc->GetName()) {
                npc->fullName = name;
            }
            if (a_ref) {
                // The reference needs its own display name (ExtraTextDisplayData) like the script's
                // SetDisplayName gave it; the base name alone was not enough for the crosshair "Talk" text.
                // Set through SKSE's Papyrus SetDisplayName (the game's own rename); C++ fallback when the
                // VM can't be called (loading).
                // Unused slots are left alone unless they still show an old follower name (after Dismiss).
                const char* shown = actor->GetDisplayFullName();
                const bool  own = actor->extraList.HasType(RE::ExtraDataType::kTextDisplayData);
                bool        active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[a_slot].Active();
                }
                const bool differs = !shown || name != shown;
                if (active ? (!own || differs) : (own && differs)) {
                    if (!Papyrus::CallMethod(actor, "ObjectReference", "SetDisplayName", {}, RE::BSFixedString(name), true)) {
                        actor->SetDisplayName(name, true);
                    }
                }
            }
        }

        // The slot's sex on its base NPC (NG: any slot can be male or female). Not in the save; the next 3D
        // load / preset apply (LoadCharacterEx resets the 3D) builds the body with it.
        void ApplySex(int a_slot)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc || MuManaged(actor)) {
                return;  // Mu Dynamic NPC Replacer sets the sex of its NPCs
            }
            bool female = false;
            {
                std::lock_guard lock(g_lock);
                const auto&     s = g_slots[a_slot];
                female = s.Active() ? s.female : s.defaultFemale;
            }
            if (npc->IsFemale() != female) {
                if (female) {
                    npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
                } else {
                    npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
                }
            }
        }

        // NG 1.2 (user: a modded voice, e.g. JJSofiaVoiceType, and the follower has no "Talk" at all).
        // Follower dialogue (vanilla, RDO...) is only offered to voice types in voice-type form lists
        // (GetIsVoiceType <list>, e.g. Skyrim.esm VoicesFollowerAll 0C78E7); a custom voice is in none. Such a
        // voice is added to every form list that holds FemaleEvenToned (13ADD) / MaleEvenToned (13AD2), so the
        // follower gets EvenToned's dialogue (lines without audio of that voice show subtitles only). Form-list
        // additions are not saved by us: done again after every load (g_dialogueVoices is cleared then).
        std::unordered_set<RE::FormID> g_dialogueVoices;  // main thread: voices already handled this load

        void GiveFollowerDialogue(RE::BGSVoiceType* a_voice, bool a_female)
        {
            if (!a_voice || g_dialogueVoices.contains(a_voice->GetFormID())) {
                return;
            }
            g_dialogueVoices.insert(a_voice->GetFormID());
            auto all = Lookup<RE::BGSListForm>(0x0C78E7);  // VoicesFollowerAll
            auto tmpl = Lookup<RE::BGSVoiceType>(a_female ? 0x013ADD : 0x013AD2);
            auto dh = RE::TESDataHandler::GetSingleton();
            if (!all || !tmpl || !dh || all->HasForm(a_voice)) {
                return;  // a vanilla follower voice already has follower dialogue
            }
            int n = 0;
            for (auto list : dh->GetFormArray<RE::BGSListForm>()) {
                if (list && list->HasForm(tmpl) && !list->HasForm(a_voice)) {
                    list->AddForm(a_voice);
                    ++n;
                }
            }
            Out(std::format("Voice {:08X}: added to {} voice lists like {} (follower dialogue)", a_voice->GetFormID(), n, a_female ? "FemaleEvenToned" : "MaleEvenToned"));
        }

        // NG 1.2 (user video 2026-11-11 10-08-49: Sofia's body stayed dark with Ciel's preset): a real NPC can have
        // its own skin (NPC WNAM, Sofia: 020758C5), which wins over the race's skin - the preset race's body
        // textures were never used. While a real NPC entry has a preset its own skin is off (the race's skin is
        // used); otherwise (Remove, revert) it is back. Base NPC edit: not saved, set again on every load.
        constexpr float kSlotHeight = 1.0f;  // NAM6 of the 100 slot NPCs in LazyFollowers.esp

        void ApplyRealSkin(int a_slot)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                return;
            }
            RE::TESObjectARMO* skin = nullptr;
            {
                std::lock_guard lock(g_lock);
                const auto&     s = g_slots[a_slot];
                if (!s.real || !s.Active()) {
                    return;  // its own skin comes back with RestoreRealHead (while its head is not shown)
                }
                skin = nullptr;
            }
            if (npc->skin != skin) {
                npc->skin = skin;
            }
            npc->defaultOutfit = nullptr;  // NG 1.2: it wears the follower's gear, the game must not dress it again
            // NG 1.2 (user: a real NPC must be as tall as the follower with the same preset): the slot NPCs' height
            // (LazyFollowers.esp NAM6, 1.0 on all 100) instead of its own (Sofia: 1.03); race and preset scale do the
            // rest as on the slot. Its own height comes back with RestoreHeadData (Remove / another save).
            npc->height = kSlotHeight;
        }

        // NG 1.2 (user: black hair on followers like Ciel): the preset's hair colour (.jslot actor.hairColor) is set by
        // us on the base NPC - one colour form per follower (a real NPC kept its own colour form, head-mesh presets
        // shared RaceMenu's one) - and on the shown hair after every preset pass (a_shown). Base NPC edit, not saved:
        // set again by every ApplyBase. No preset: a slot's own colour back (real NPCs: RestoreHeadData).
        std::unordered_map<int, RE::BGSColorForm*> g_hairForms;  // main thread: slot -> its colour form


        void ApplyPresetHair(int a_slot, bool a_shown)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                return;
            }
            std::string preset;
            {
                std::lock_guard lock(g_lock);
                const auto&     s = g_slots[a_slot];
                if (!s.Active()) {
                    if (!s.real && npc->headRelatedData) {
                        npc->SetHairColor(Lookup<RE::BGSColorForm>(s.defaultHair));
                    }
                    return;
                }
                preset = s.preset;
            }
            const auto color = Files::PresetHairColor(preset + ".jslot");
            if (!color) {
                return;
            }
            auto& form = g_hairForms[a_slot];
            if (!form) {
                if (auto factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::BGSColorForm>()) {
                    form = factory->Create();
                }
            }
            if (!form) {
                return;
            }
            form->color = RE::Color(static_cast<std::uint8_t>(*color >> 16), static_cast<std::uint8_t>(*color >> 8), static_cast<std::uint8_t>(*color), 0);
            if (!npc->headRelatedData || npc->headRelatedData->hairColor != form) {
                npc->SetHairColor(form);
            }
            if (a_shown && actor->Is3DLoaded()) {
                actor->UpdateHairColor();
            }
        }

        // Name, voice, combat style, class on the base NPC (the scripts' checkmyPresetName / checkMyVoice /
        // SetCombatStyle / SetClass) and the sex (NG). None of these are in the save.
        void ApplyBase(int a_slot, bool a_ref = true)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                return;
            }
            Slot s;
            {
                std::lock_guard lock(g_lock);
                s = g_slots[a_slot];
            }
            ApplyName(a_slot, a_ref);
            ApplySex(a_slot);
            ApplyPresetHair(a_slot, false);
            if (s.real) {
                ApplyRealSkin(a_slot);
                // NG 1.2 (user): a real NPC's voice can be changed too
                if (auto v = Lookup<RE::BGSVoiceType>(s.Active() ? s.voice : s.defaultVoice)) {
                    npc->voiceType = v;
                }
                // NG (user: "the user loses the ability to add skill, class, etc. on the Override follower"): combat style
                // and class like a slot's (Behaviour section); its own ones until one is picked and again when given back
                if (auto cs = Lookup<RE::TESCombatStyle>(s.Active() && g_stylesEnabled ? s.style : s.defaultStyle)) {
                    npc->combatStyle = cs;
                }
                if (auto c = Lookup<RE::TESClass>(s.Active() && g_classesEnabled ? s.npcClass : s.defaultClass)) {
                    npc->npcClass = c;
                }
                return;
            }
            if (auto v = Lookup<RE::BGSVoiceType>(s.Active() ? s.voice : s.defaultVoice)) {
                npc->voiceType = v;
                if (s.Active()) {
                    GiveFollowerDialogue(v, s.female);
                    Voice::Prepare(v);  // NG 1.2: its mod's lines, swapped in by Voice's hook
                }
            }
            if (auto cs = Lookup<RE::TESCombatStyle>(s.Active() && g_stylesEnabled ? s.style : s.defaultStyle)) {
                npc->combatStyle = cs;
            }
            if (auto c = Lookup<RE::TESClass>(s.Active() && g_classesEnabled ? s.npcClass : s.defaultClass)) {
                npc->npcClass = c;
            }
        }

        // The scripts' checkRelationship: relationship rank with the player at least 3 (ally).
        void CheckRelationship(RE::Actor* a_actor)
        {
            auto player = RE::PlayerCharacter::GetSingleton();
            Papyrus::CallMethod(a_actor, "Actor", "GetRelationshipRank", [a_actor, player](const Papyrus::Variable& a_rank) {
                if (a_rank.IsInt() && a_rank.GetSInt() < 3) {
                    Papyrus::CallMethod(a_actor, "Actor", "SetRelationshipRank", {}, static_cast<RE::Actor*>(player), std::int32_t(3));
                }
            }, static_cast<RE::Actor*>(player));
        }

        // The scripts' refreshAppearance(fullRefresh): race, then the RaceMenu preset (full = menu flags,
        // lite = the flags used when an NPC loads; head-mesh presets use the preset / external functions),
        // then LoadCharacterEx with flag 2 (the scripts' "tattoo fix"). Runs as one Papyrus chain.
        // a_playerFace: afterwards refresh the player's face (after Apply / Summon, see RefreshPlayerFace).
        // a_body (NG, user): the preset's body morphs (flag 2) only in the pass right after a full apply; every
        // other lite pass (loads, summons) leaves the body alone, so a body changed later (OBody, RaceMenu
        // sliders - RaceMenu saves those itself) is kept instead of going back to the preset's.
        // a_then (main thread): runs once the whole chain has finished (Apply Settings: then summon).
        // NG: one pass per follower at a time (see kSettle); a request while one runs is merged and runs after
        // it. Followers whose look is Mu Dynamic NPC Replacer's get no pass (a_then still runs).
        void FinishPass(int a_slot, unsigned a_gen);

        // NG fix (user screenshot 2026-11-12: the taken-over Sofia had Ciel's face but no eye make-up / eyeliner).
        // A preset's tint layers (eyeliner, eye sockets, lips...) are the preset's tint .dds
        // (Textures\CharGen\Exported\<preset>.dds), which RaceMenu only puts on the face in its RegenerateHead hook
        // (PresetInterface::ApplyMappedPreset); on a real NPC the face kept another tint. Puts the .dds on the face
        // as a RaceMenu node override (texture 6 = tint, persistent: back on every 3D load, removed by Remove's
        // RemoveAllReferenceNodeOverrides). Main thread; a_done runs once in every case.
        void SetRealFaceTint(RE::Actor* a_actor, const std::string& a_preset, const std::string& a_name, Papyrus::Done a_done)
        {
            const std::string tint = "Textures\\CharGen\\Exported\\" + a_preset + ".dds";
            std::error_code   ec;
            auto              npc = a_actor->GetActorBase();
            auto              obj = a_actor->Is3DLoaded() ? a_actor->GetHeadPartObject(RE::BGSHeadPart::HeadPartType::kFace) : nullptr;
            if (!std::filesystem::exists(std::filesystem::path("Data") / tint, ec) || !npc || !obj || obj->name.empty()) {
                Out(std::format("{} - face tint not set ({})", a_name, !obj ? "no loaded face" : "no " + tint));
                a_done(Papyrus::Variable{});
                return;
            }
            std::string shown = "none";
            if (auto geo = obj->AsGeometry()) {
                auto prop = geo->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get();
                auto shader = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
                auto mat = shader ? shader->material : nullptr;
                if (mat && mat->GetFeature() == RE::BSShaderMaterial::Feature::kFaceGen) {
                    if (auto t = static_cast<RE::BSLightingShaderMaterialFacegen*>(mat)->tintTexture.get(); t && !t->name.empty()) {
                        shown = t->name.c_str();
                    }
                }
            }
            Out(std::format("{} - face tint {} -> {} (face {})", a_name, shown, tint, obj->name.c_str()));
            if (!Papyrus::CallStatic("NiOverride", "AddNodeOverrideString", a_done, static_cast<RE::TESObjectREFR*>(a_actor), npc->IsFemale(),
                    RE::BSFixedString(obj->name), std::int32_t(9), std::int32_t(6), RE::BSFixedString(tint), true)) {
                a_done(Papyrus::Variable{});
            }
        }

        // NG (user: the Erin race's extra preset - extra tints, eye colour set, skin, head-part adjust - is not in the
        // .jslot; Mu Dynamic NPC Replacer loads it through the Erin race's interface). REA of MuErinRace.dll 1.4.28:
        // its SKSE listener (any sender) answers a message of type 'MUER' by writing its interface
        // (ERIN::ErinRaceInterface) into the pointer the message's data points to. Its vtable: [1] = version (6),
        // [3] = ExtraPreset::LoadErinExtraPresetForActor(actor, name, all parts 0x1F) -> bool; the name is a path
        // relative to Data/SKSE/Plugins (".ini" added if missing). The Erin race keeps it in its own co-save.
        void*                   g_erin = nullptr;
        constexpr std::uint32_t kErinRequest = 'MUER';
        constexpr std::uint32_t kErinMinVersion = 6;

        // The slot's Erin Preset on its actor (every pass, after the RaceMenu preset). Main thread, 3D loaded only.
        void ApplyErinPreset(int a_slot)
        {
            std::string file, name;
            {
                std::lock_guard lock(g_lock);
                file = g_slots[a_slot].erinPreset;
                name = g_slots[a_slot].name;
            }
            auto actor = ActorOf(a_slot);
            if (!g_erin || file.empty() || !actor || !actor->Is3DLoaded()) {
                return;
            }
            const auto path = "ErinRace\\Preset\\" + file;
            auto       vtbl = *reinterpret_cast<void***>(g_erin);
            // NG fix (user: Ciel lost her skin once Mu Dynamic NPC Replacer was off; ErinRace.log "Ciel is not erin"):
            // Erin's isErinActor keeps its answer per actor (ActorManager map, REA) - it was asked while the NPC
            // still had its own race (Sofia = not Erin), so the Erin skin never went on her body. [5] = re-verify
            // (RemoveErinActorMap drops that answer, Erin checks the actor again with the race it has now); the
            // preset after it registers the tints / skin again.
            reinterpret_cast<void (*)(void*, RE::Actor*)>(vtbl[5])(g_erin, actor);
            const bool ok = reinterpret_cast<bool (*)(void*, RE::Actor*, const char*)>(vtbl[3])(g_erin, actor, path.c_str());
            if (ok) {
                Out(std::format("{} - Erin preset {} loaded", name, file));
            } else {
                logger::warn("{} - Erin preset {} could not be loaded (see ErinRace.log)", name, file);
            }
        }

        void RefreshAppearance(int a_slot, bool a_full, bool a_playerFace = false, bool a_body = false, std::function<void()> a_then = {})
        {
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            Slot s;
            {
                std::lock_guard lock(g_lock);
                s = g_slots[a_slot];
                if (g_passes.size() < g_slots.size()) {
                    g_passes.resize(g_slots.size());
                    g_resetLook.resize(g_slots.size(), 0);
                }
                if (g_freshUntil.size() < g_slots.size()) {
                    g_freshUntil.resize(g_slots.size());
                }
            }
            if (!s.Active()) {
                return;
            }
            if (MuManaged(actor)) {
                Out(std::format("{} - look left to Mu Dynamic NPC Replacer (its keyword is on the NPC)", s.name));
                g_resetLook[a_slot] = 0;
                if (a_then) {
                    a_then();
                }
                return;
            }
            const unsigned gen = g_generation.load();
            auto&          ps = g_passes[a_slot];
            const auto     now = std::chrono::steady_clock::now();
            if (ps.running && ps.gen == gen && now - ps.started < std::chrono::seconds(30)) {  // 30 s: a pass that never reported back
                ps.pending = true;
                ps.full = ps.full || a_full;
                ps.playerFace = ps.playerFace || a_playerFace;
                ps.body = ps.body || a_body;
                if (a_then) {
                    ps.thens.push_back(std::move(a_then));
                }
                return;
            }
            // NG 1.2 (user: "a lot of rebuilding"): a lite pass (no body) right after this follower's full apply
            // (summon arrival, its 3D load) does not load the preset again - the full apply just did, and RaceMenu
            // restores its data on a 3D load. Only the cheap part: overlays again + the player's face.
            // NG (user): one RaceMenu Mode for every pass (the esp's RMLiteMode global; RMFullMode is unused)
            const auto fullMode = std::int32_t(GetGlobal(Global::kLiteMode));
            const auto liteMode = std::int32_t(a_body ? fullMode : (fullMode & ~2));
            if (!a_full && !a_body && a_slot < static_cast<int>(g_freshUntil.size()) && now < g_freshUntil[a_slot] && ps.gen == gen) {
                if (liteMode & 1) {
                    Papyrus::CallStatic("NiOverride", "ApplyNodeOverrides", {}, static_cast<RE::TESObjectREFR*>(actor));
                }
                RefreshPlayerFace();
                Out(std::format("{} - preset just applied, no reload (overlays + player face only)", s.name));
                if (a_then) {
                    a_then();
                }
                return;
            }
            ps = PassState{};
            ps.running = true;
            ps.gen = gen;
            ps.started = now;

            auto chain = Papyrus::Chain::Make();
            // NG fix (crash 2026-11-12 14:40:57, user video 14-40-11: Override moved from Ciel to Vicky - Sofia, just
            // given back with her own head, got Vicky's race while shown; the head rebuilt from her head part mesh
            // SofiaHeadHP and face animation (MuFacialExpressionExtended) crashed on it, as in (70) / (76)): a shown
            // real NPC is disabled BEFORE race + preset (like Remove's "head taken away first") and enabled after.
            auto hidden = std::make_shared<bool>(false);
            if (a_full && s.real) {
                chain->Then([actor, hidden](Papyrus::Done d) {
                    *hidden = actor->Is3DLoaded() && !actor->IsDisabled();
                    return *hidden && Papyrus::CallMethod(actor, "ObjectReference", "Disable", d, false);
                });
                chain->Then([hidden](Papyrus::Done d) { return *hidden && WaitStep(kSettle)(d); });
            }
            // Apply Settings (user: a preset applied with the wrong race needed Remove to fix): first
            // clear the base NPC's RaceMenu preset (Reset's appearance part). NG 1.2: no detour through the slot's
            // own race any more (one head rebuild less); the SetRace below still changes a wrong race.
            if (a_full && g_resetLook[a_slot]) {
                g_resetLook[a_slot] = 0;
                if (auto npc = actor->GetActorBase()) {
                    chain->Then([npc](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "ClearPreset", d, npc); });
                }
            }
            auto race = Lookup<RE::TESRace>(s.race);
            if (!race) {
                race = actor->GetRace();
            }
            const RE::BSFixedString preset(s.preset);
            // RaceMenu's LoadCharacterPresetEx reads SKSE\Plugins\CharGen\Presets\<name>.jslot (source:
            // PapyrusCharGen.cpp), not Exported like the other loaders: our presets live in Exported, so that call
            // gets "../Exported/<name>" (what the old mod passed; only the tint path broke with the prefix).
            const RE::BSFixedString presetFromPresets("../Exported/" + s.preset);
            // NG fix: never the head-mesh loaders on a real NPC - LoadExternalCharacterEx copies the preset's .nif /
            // .dds over FaceGeom / FaceTint\<the NPC's plugin>\<id> on disk, i.e. the NPC mod's own face files
            const bool head = !s.headMesh.empty() && !s.real;
            auto                    hair = g_hairColor;

            // the race check runs when the chain gets there (the reset above may have changed it); after a
            // SetRace wait for the new head before loading the preset
            auto raceSet = std::make_shared<bool>(false);
            chain->Then([actor, race, raceSet](Papyrus::Done d) {
                if (actor->GetRace() == race) {
                    return false;
                }
                *raceSet = Papyrus::CallMethod(actor, "Actor", "SetRace", d, race);
                return *raceSet;
            });
            chain->Then([raceSet](Papyrus::Done d) { return *raceSet && WaitStep(kSettle)(d); });
            // NG 1.2: a real NPC's body is rebuilt before a full apply, so the race skin that replaced its own skin
            // (ApplyRealSkin) shows at once. Before, not after (user: hair colour lost right after Apply - the
            // rebuild after the preset load put the NPC's own hair colour back).
            if (a_full && s.real) {
                chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "QueueNiNodeUpdate", d); });
                chain->Then(WaitStep(kSettle));
            }
            // NG 1.2 fix (user: small head on a summoned follower - Ciel.jslot scales "NPC Head" x1.4, a
            // RaceMenu transform, mode bit 4): the lite pass of a summon started a few ms after the MoveTo, before
            // the follower's 3D was there, so RaceMenu had no skeleton to put the preset's transforms on. A lite
            // pass now waits for the 3D first (5 s at most).
            if (!a_full) {
                chain->Then([actor](Papyrus::Done d) {
                    WaitLoaded(actor, 20, d);
                    return true;
                });
            }
            const std::string what = a_full ? (head ? "Headmesh Full" : "Full") : (head ? "HeadMesh Lite" : "Lite");
            chain->ThenDo([what, name = s.name, p = s.preset]() { Out(std::format("{} - applying preset - {} - {}", name, what, p)); });
            if (a_full) {
                // NG fix (user: "sometimes the belly protrudes"): preset body morphs are saved under the key of
                // whoever made them (Penny.jslot: "OBody"); morphs already on the actor that the preset does not
                // list (OBody's random body on a fresh slot) stayed and added up. A full apply with Body (2)
                // first clears the actor's body morphs, like Mu Dynamic NPC Replacer does before its apply.
                if (fullMode & 2) {
                    chain->Then([actor](Papyrus::Done d) {
                        return Papyrus::CallStatic("NiOverride", "ClearMorphs", d, static_cast<RE::TESObjectREFR*>(actor));
                    });
                }
                if (head) {
                    chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadCharacterPresetEx", d, actor, presetFromPresets, hair, fullMode); });
                } else {
                    chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadCharacterEx", d, actor, race, preset, fullMode); });
                }
            } else {
                if (head) {
                    chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadExternalCharacterEx", d, actor, race, preset, liteMode); });
                } else {
                    chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadCharacterEx", d, actor, race, preset, liteMode); });
                }
            }
            // NG fix (user video 2026-11-12 10-37-01: with Override on, the taken-over Sofia kept her own look - nothing
            // of Ciel.jslot showed - while Sofia copies placed with ModEx had it): a slot always arrives from the
            // storage cell with a new 3D, a real NPC keeps the 3D it had, and the head / body rebuild the preset load
            // queues did not show on it. After a full apply a shown real NPC is disabled and enabled again (as
            // Remove does), so its head and body are built new from the preset; then wait for the new 3D.
            if (a_full && s.real) {
                // NG fix (user picture 2026-11-12, Override ticked: Sofia taken over while far away showed Ciel's head
                // but not her body, name "Sofia"; log "face tint not set (no loaded face)"): the rebuild and tint need
                // her 3D. Not loaded -> she is moved in front of the player (as the summon after the apply would) and
                // the pass waits for her 3D (10 s at most). Only AFTER race + preset (crash 2026-11-12 12:54:26: moved in
                // before them, she loaded half-changed with her own SofiaHeadHP mesh and face animation crashed).
                // NG (user): a new game's kept take-over without Pre Summon stays where it is - not brought in;
                // its next 3D load gets the full apply (rebuild + tint need the 3D)
                const bool stay = g_stayPut.erase(a_slot) > 0;
                auto moved = std::make_shared<bool>(false);
                chain->Then([actor, moved, stay, a_slot, hidden](Papyrus::Done d) {
                    auto player = RE::PlayerCharacter::GetSingleton();
                    if (*hidden || !player || (actor->Is3DLoaded() && !actor->IsDisabled())) {
                        return false;  // hidden by us above (it is near): Enable below
                    }
                    if (stay) {
                        OnMain([a_slot]() {
                            if (a_slot < static_cast<int>(g_fullOnLoad.size())) {
                                g_fullOnLoad[a_slot] = 1;
                            }
                        });
                        return false;
                    }
                    const float angle = player->GetAngleZ();
                    *moved = Papyrus::CallMethod(actor, "ObjectReference", "MoveTo", d, static_cast<RE::TESObjectREFR*>(player),
                        120.0f * std::sin(angle), 120.0f * std::cos(angle), 0.0f, true);
                    return *moved;
                });
                chain->Then([actor, moved](Papyrus::Done d) {
                    if (!*moved) {
                        return false;
                    }
                    WaitLoaded(actor, 40, d);
                    return true;
                });
                chain->ThenDo([actor, moved, name = s.name]() {
                    if (*moved) {
                        Out(std::format("{} - was not loaded: brought to the player after the preset (3D loaded {})", name, actor->Is3DLoaded()));
                    }
                });
                // hidden before race + preset: shown again now - its new 3D is built from the preset (no second rebuild)
                chain->Then([actor, hidden](Papyrus::Done d) { return *hidden && Papyrus::CallMethod(actor, "ObjectReference", "Enable", d, false); });
                chain->Then([actor, hidden](Papyrus::Done d) {
                    if (!*hidden) {
                        return false;
                    }
                    WaitLoaded(actor, 20, d);
                    return true;
                });
                chain->ThenDo([name = s.name, hidden]() {
                    if (*hidden) {
                        Out(std::format("{} - hidden during race + preset, shown again (real NPC)", name));
                    }
                });
                auto shown = std::make_shared<bool>(false);
                chain->Then(WaitStep(kSettle));
                chain->Then([actor, shown, hidden](Papyrus::Done d) {
                    *shown = !*hidden && actor->Is3DLoaded() && !actor->IsDisabled();
                    return *shown && Papyrus::CallMethod(actor, "ObjectReference", "Disable", d, false);
                });
                chain->Then([shown](Papyrus::Done d) { return *shown && WaitStep(kSettle)(d); });
                chain->Then([actor, shown](Papyrus::Done d) { return *shown && Papyrus::CallMethod(actor, "ObjectReference", "Enable", d, false); });
                chain->Then([actor, shown](Papyrus::Done d) {
                    if (!*shown) {
                        return false;
                    }
                    WaitLoaded(actor, 20, d);
                    return true;
                });
                chain->ThenDo([name = s.name, shown]() {
                    if (*shown) {
                        Out(std::format("{} - 3D rebuilt after the preset (real NPC)", name));
                    }
                });
                // the preset's make-up (tint .dds) on the new face, see SetRealFaceTint
                chain->Then([actor, preset = s.preset, name = s.name](Papyrus::Done d) {
                    OnMain([actor, preset, name, d]() { SetRealFaceTint(actor, preset, name, d); });
                    return true;
                });
            }
            // NG 1.2: the old mod's second LoadCharacterEx (flag 2, "tattoo fix") after a full apply is gone - it
            // only forced another head rebuild; the body comes with the mode's own Body bit and the overlays with
            // ApplyNodeOverrides below.
            // NG fix (user: face paint / makeup overlays sometimes missing after a load or Apply Settings).
            // RaceMenu (REA, ApplyPreset FUN_180083080) with Overrides (1): removes the actor's node overrides,
            // queues SKSETaskRevertOverlays, then stores the preset's overrides again. AddOverlays (which
            // re-applies) is only queued when the actor has no overlays yet - so from the second load on, the
            // revert task runs after the new data and blanks the overlays until the next 3D reset. Once the
            // head has settled, apply the stored node overrides again.
            // Same for the transforms (4, small head fix above): once the head has settled, the stored node
            // transforms (head / bone scales) are put on the shown skeleton again.
            const auto mode = a_full ? fullMode : liteMode;
            if (mode & 5) {
                chain->Then(WaitStep(kSettle));
            }
            if (mode & 1) {
                chain->Then([actor](Papyrus::Done d) {
                    return Papyrus::CallStatic("NiOverride", "ApplyNodeOverrides", d, static_cast<RE::TESObjectREFR*>(actor));
                });
            }
            if (mode & 4) {
                chain->Then([actor](Papyrus::Done d) {
                    return actor->Is3DLoaded() && Papyrus::CallStatic("NiOverride", "UpdateAllReferenceTransforms", d, static_cast<RE::TESObjectREFR*>(actor));
                });
            }
            // SetRace / the preset can drop the display name: set it again at the end
            chain->ThenDo([a_slot]() {
                OnMain([a_slot]() {
                    ApplyName(a_slot);
                    ApplyPresetHair(a_slot, true);  // NG 1.2: the preset's hair colour on the shown hair
                    ApplyErinPreset(a_slot);        // NG (user): the Erin race's extra preset after the RaceMenu one
                });
            });
            // NG 1.2 (user: the player's skin / face went dark again): every preset load on a follower can cost
            // the PLAYER its textures (RaceMenu side effect), not only the ones after Apply / Summon - so every
            // pass ends with the (debounced) player refresh. The old follow-up lite pass after a full apply is
            // gone: the full apply already uses the RaceMenu Mode (overlays / skin bits included).
            // NG fix (user: "a lot of re-apply" - log 16:00:28 / 16:00:31: a summoned follower got two Lite reloads, one
            // from its 3D load event and one from the summon's arrival): any pass that ended with the follower's 3D
            // there (full, or lite - which waits for the 3D first) counts as "just applied" for 8 s, not only a full one
            chain->ThenDo([a_slot]() {
                OnMain([a_slot]() {
                    RefreshPlayerFace();
                    auto a = ActorOf(a_slot);
                    if (a && a->Is3DLoaded() && a_slot < static_cast<int>(g_freshUntil.size())) {
                        g_freshUntil[a_slot] = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                    }
                });
            });
            if (a_then) {
                chain->ThenDo([a_then]() { OnMain(a_then); });
            }
            // the next pass of this follower may start kSettle seconds after this one
            chain->ThenDo([a_slot, gen]() { After(kSettle, [a_slot, gen]() { FinishPass(a_slot, gen); }); });
            chain->Run();
        }

        // A pass is over: run the merged request that came meanwhile, if any.
        void FinishPass(int a_slot, unsigned a_gen)
        {
            if (a_slot < 0 || a_slot >= static_cast<int>(g_passes.size())) {
                return;
            }
            auto& ps = g_passes[a_slot];
            if (ps.gen != a_gen || !ps.running) {
                return;
            }
            PassState next = std::move(ps);
            ps = PassState{};
            // NG fix (user: "a lot of re-applying" after an Override apply - log: Full, then 2 more Lite passes): keep
            // the game's generation, else RefreshAppearance's "preset just applied, no reload" check (ps.gen == gen)
            // failed for every request merged while the full pass ran (summon, 3D load events)
            ps.gen = a_gen;
            if (!next.pending) {
                return;
            }
            std::function<void()> then;
            if (!next.thens.empty()) {
                then = [thens = std::move(next.thens)]() {
                    for (auto& t : thens) {
                        t();
                    }
                };
            }
            RefreshAppearance(a_slot, next.full, next.playerFace, next.body, std::move(then));
        }

        // The scripts' updateMyData: for an active, enabled follower re-apply name / voice / relationship /
        // style / class and the preset (lite unless a_full).
        void UpdateFollower(int a_slot, bool a_full, bool a_playerFace = false, bool a_body = false)
        {
            auto actor = ActorOf(a_slot);
            {
                std::lock_guard lock(g_lock);
                if (!actor || a_slot >= static_cast<int>(g_slots.size()) || !g_slots[a_slot].Active()) {
                    return;
                }
            }
            if (actor->IsDisabled()) {
                return;
            }
            ApplyBase(a_slot);
            if (!IsReal(a_slot)) {
                CheckRelationship(actor);  // a real NPC's relationship is its own mod's business
            }
            RefreshAppearance(a_slot, a_full, a_playerFace, a_body);
        }

        // OnLoad of the actor script: a moment after the follower loads, update it. NG (user): the Apply Delay setting
        // is gone - a fixed 1 s (the esp's default); the lite pass also waits for the 3D itself (WaitLoaded).
        constexpr float kLoadDelay = 1.0f;

        void ScheduleUpdate(int a_slot, bool a_full, bool a_playerFace, bool a_body)
        {
            After(kLoadDelay, [a_slot, a_full, a_playerFace, a_body]() { UpdateFollower(a_slot, a_full, a_playerFace, a_body); });
        }

        // Spells: add / remove on the actor (the game saves added spells itself).
        void RemoveSlotSpells(int a_slot)
        {
            auto actor = ActorOf(a_slot);
            std::vector<RE::FormID> spells;
            {
                std::lock_guard lock(g_lock);
                spells.swap(g_slots[a_slot].spells);
            }
            if (!actor) {
                return;
            }
            for (auto id : spells) {
                if (auto sp = Lookup<RE::SpellItem>(id)) {
                    actor->RemoveSpell(sp);
                }
            }
        }

        void AddSlotSpell(int a_slot, RE::SpellItem* a_spell)
        {
            auto actor = ActorOf(a_slot);
            if (!actor || !a_spell) {
                return;
            }
            std::lock_guard lock(g_lock);
            auto&           list = g_slots[a_slot].spells;
            if (std::find(list.begin(), list.end(), a_spell->GetFormID()) == list.end()) {
                list.push_back(a_spell->GetFormID());
                actor->AddSpell(a_spell);
            }
        }

        // The Papyrus actor value names of the Stats page (stats, then skills).
        RE::ActorValue StatAV(std::string_view a_name)
        {
            for (auto& [n, av] : kStats) {
                if (a_name == n) {
                    return av;
                }
            }
            for (auto& [n, av] : kSkills) {
                if (a_name == n) {
                    return av;
                }
            }
            return RE::ActorValue::kNone;
        }

        // Stats page values back to what they were before they were changed (a_name empty = all of them).
        void RestoreStats(int a_slot, const std::string& a_name = {})
        {
            auto                         actor = ActorOf(a_slot);
            std::map<std::string, float> restore;
            {
                std::lock_guard lock(g_lock);
                auto&           d = g_slots[a_slot].statDefaults;
                if (a_name.empty()) {
                    restore.swap(d);
                } else if (auto it = d.find(a_name); it != d.end()) {
                    restore.insert(*it);
                    d.erase(it);
                }
            }
            for (auto& [name, value] : restore) {
                if (actor) {
                    Papyrus::CallMethod(actor, "Actor", "ForceActorValue", {}, RE::BSFixedString(name), value);
                }
            }
        }

        // NG 1.2 (user video 2026-11-11 10-17-22: Severa's face broken after Remove, mouth wide open): a RaceMenu
        // preset changes the base NPC's head parts, face morphs, hair colour, face texture, height and weight in
        // memory; ClearPreset does not put them back, so her own face mesh was built with Ciel's head parts.
        // Every unique NPC's own head data is kept as loaded (kDataLoaded, before any preset) and written back by
        // Remove / before another save is loaded (RestoreHeadData). Tint layers are not kept (NPCs with a face
        // mesh of their own use its baked texture).
        struct HeadData
        {
            std::vector<RE::BGSHeadPart*> parts;
            bool                          hasFace = false;
            RE::TESNPC::FaceData          face{};
            bool                          hasHead = false;
            RE::BGSColorForm*             hairColor = nullptr;
            RE::BGSTextureSet*            faceDetails = nullptr;
            float                         height = 1.0f;
            float                         weight = 50.0f;
            RE::Color                     bodyTint{};
            RE::TESNPC*                   faceNPC = nullptr;
            RE::TESObjectARMO*            skin = nullptr;    // NG fix: its own skin / outfit / voice as loaded (see OwnDefaults)
            RE::BGSOutfit*                outfit = nullptr;
            RE::BGSVoiceType*             voice = nullptr;
            RE::TESCombatStyle*           style = nullptr;   // NG (user: style / class on a taken-over NPC): its own ones
            RE::TESClass*                 npcClass = nullptr;
        };
        std::unordered_map<RE::FormID, HeadData> g_headData;  // base NPC -> its own head data (written at kDataLoaded only)

        // kDataLoaded: the head data of every unique NPC (the player excluded).
        void KeepHeadData()
        {
            auto dh = RE::TESDataHandler::GetSingleton();
            if (!dh) {
                return;
            }
            for (auto npc : dh->GetFormArray<RE::TESNPC>()) {
                if (!npc || !npc->IsUnique() || npc->GetFormID() == 0x7) {
                    continue;
                }
                HeadData h;
                for (std::int8_t i = 0; npc->headParts && i < npc->numHeadParts; ++i) {
                    h.parts.push_back(npc->headParts[i]);
                }
                if (npc->faceData) {
                    h.hasFace = true;
                    h.face = *npc->faceData;
                }
                if (npc->headRelatedData) {
                    h.hasHead = true;
                    h.hairColor = npc->headRelatedData->hairColor;
                    h.faceDetails = npc->headRelatedData->faceDetails;
                }
                h.height = npc->height;
                h.weight = npc->weight;
                h.bodyTint = npc->bodyTintColor;
                h.faceNPC = npc->faceNPC;
                h.skin = npc->skin;
                h.outfit = npc->defaultOutfit;
                h.voice = npc->voiceType;
                h.style = npc->combatStyle;
                h.npcClass = npc->npcClass;
                g_headData.emplace(npc->GetFormID(), std::move(h));
            }
            logger::info("Head data of {} unique NPCs kept (real NPC followers: Remove gives it back)", g_headData.size());
        }

        // NG fix: a real NPC entry's own skin, outfit, voice (and combat style, class). Read from the kDataLoaded snapshot when there is one:
        // the base NPC itself may still carry our edits (a save reloaded in the same session while its 3D was
        // loaded keeps skin = none, so Remove would have left it the race's skin). Else from the base NPC.
        void OwnDefaults(const RE::TESNPC* a_npc, Slot& a_slot)
        {
            if (auto it = g_headData.find(a_npc->GetFormID()); it != g_headData.end()) {
                a_slot.defaultSkin = IdOf(it->second.skin);
                a_slot.defaultOutfit = IdOf(it->second.outfit);
                a_slot.defaultVoice = IdOf(it->second.voice);
                a_slot.defaultStyle = IdOf(it->second.style);
                a_slot.defaultClass = IdOf(it->second.npcClass);
                return;
            }
            a_slot.defaultSkin = IdOf(a_npc->skin);
            a_slot.defaultOutfit = IdOf(a_npc->defaultOutfit);
            a_slot.defaultVoice = IdOf(a_npc->voiceType);
            a_slot.defaultStyle = IdOf(a_npc->combatStyle);
            a_slot.defaultClass = IdOf(a_npc->npcClass);
        }

        // The kept head data back on a_npc (main thread). False = none kept (not a unique NPC).
        bool RestoreHeadData(RE::TESNPC* a_npc)
        {
            auto it = a_npc ? g_headData.find(a_npc->GetFormID()) : g_headData.end();
            if (it == g_headData.end()) {
                return false;
            }
            const auto& h = it->second;
            // a new array (the current one may be RaceMenu's; left alone, a few bytes)
            auto parts = RE::calloc<RE::BGSHeadPart*>(std::max<std::size_t>(1, h.parts.size()));
            for (std::size_t i = 0; i < h.parts.size(); ++i) {
                parts[i] = h.parts[i];
            }
            a_npc->headParts = parts;
            a_npc->numHeadParts = static_cast<std::int8_t>(h.parts.size());
            if (a_npc->faceData) {
                if (h.hasFace) {
                    *a_npc->faceData = h.face;
                } else {
                    std::fill(std::begin(a_npc->faceData->morphs), std::end(a_npc->faceData->morphs), 0.0f);
                }
            }
            if (a_npc->headRelatedData) {
                a_npc->headRelatedData->hairColor = h.hasHead ? h.hairColor : nullptr;
                a_npc->headRelatedData->faceDetails = h.hasHead ? h.faceDetails : nullptr;
            }
            a_npc->height = h.height;
            a_npc->weight = h.weight;
            a_npc->bodyTintColor = h.bodyTint;
            a_npc->faceNPC = h.faceNPC;
            return true;
        }

        // NG 1.2: a real NPC's own skin and head data back on its base NPC. Only while its head is not shown (crash
        // 2026-11-11 10:59:50 in the face animation, MuFacialExpressionExtended -> game face morphing: the head
        // parts were swapped while the preset's head was still shown, and its morph data was read for parts that
        // were never loaded).
        void RestoreRealHead(RE::TESNPC* a_npc, RE::FormID a_skin, const std::string& a_name)
        {
            a_npc->skin = Lookup<RE::TESObjectARMO>(a_skin);
            if (!RestoreHeadData(a_npc)) {
                Out(std::format("{} - no kept head data (not a unique NPC): its face comes back after a game restart", a_name));
            }
        }

        // NG 1.2: a real NPC entry's base NPC back to the name and sex it had when it was added (Remove, and
        // before another save is loaded - base NPC edits stay in memory between loads). a_ref: also its display name.
        // a_head: also its skin + head data when its head is not shown.
        void RestoreText(RE::Actor* a_actor, const Slot& a_slot);

        void RestoreRealBase(int a_slot, bool a_ref, bool a_head)
        {
            auto actor = ActorOf(a_slot);
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                return;
            }
            std::string        name;
            bool               female = false;
            RE::TESObjectARMO* skin = nullptr;
            {
                std::lock_guard lock(g_lock);
                name = g_slots[a_slot].defaultName;
                female = g_slots[a_slot].defaultFemale;
                skin = Lookup<RE::TESObjectARMO>(g_slots[a_slot].defaultSkin);
                npc->defaultOutfit = Lookup<RE::BGSOutfit>(g_slots[a_slot].defaultOutfit);
                if (auto v = Lookup<RE::BGSVoiceType>(g_slots[a_slot].defaultVoice)) {
                    npc->voiceType = v;
                }
                if (auto cs = Lookup<RE::TESCombatStyle>(g_slots[a_slot].defaultStyle)) {
                    npc->combatStyle = cs;  // NG: its own combat style / class back (Behaviour section)
                }
                if (auto c = Lookup<RE::TESClass>(g_slots[a_slot].defaultClass)) {
                    npc->npcClass = c;
                }
            }
            (void)skin;
            if (a_head && !actor->Is3DLoaded()) {
                RestoreRealHead(npc, IdOf(skin), name);  // head not shown: safe (else RestoreRealLook does it)
            }
            if (!name.empty() && name != npc->GetName()) {
                npc->fullName = name;
            }
            bool textSaved = false;
            Slot text;
            {
                std::lock_guard lock(g_lock);
                textSaved = g_slots[a_slot].textSaved;
                if (textSaved) {
                    text = g_slots[a_slot];
                }
            }
            if (a_ref && textSaved) {
                RestoreText(actor, text);  // its own name data exactly as it was (quest / message link included)
            } else if (a_ref && actor->extraList.HasType(RE::ExtraDataType::kTextDisplayData)) {
                const char* shown = actor->GetDisplayFullName();
                if (!name.empty() && (!shown || name != shown) &&
                    !Papyrus::CallMethod(actor, "ObjectReference", "SetDisplayName", {}, RE::BSFixedString(name), true)) {
                    actor->SetDisplayName(name, true);
                }
            }
            if (!MuManaged(actor) && npc->IsFemale() != female) {
                if (female) {
                    npc->actorData.actorBaseFlags.set(RE::ACTOR_BASE_DATA::Flag::kFemale);
                } else {
                    npc->actorData.actorBaseFlags.reset(RE::ACTOR_BASE_DATA::Flag::kFemale);
                }
            }
        }

        // NG fix (RaceMenu source, PresetInterface::ApplyPreset): RaceMenu keeps body morphs, node overrides, skin
        // overrides and transforms per reference in its co-save and puts them back on every 3D load; ClearPreset
        // drops none of them, and an apply only adds skin overrides (never removes old ones). Remove clears all four,
        // so a real NPC loses e.g. the preset's head scale and a reused slot starts clean.
        void AddRaceMenuCleanup(Papyrus::Chain& a_chain, RE::Actor* a_actor)
        {
            for (const char* fn : { "ClearMorphs", "RemoveAllReferenceNodeOverrides", "RemoveAllReferenceSkinOverrides", "RemoveAllReferenceTransforms" }) {
                a_chain.Then([a_actor, fn](Papyrus::Done d) { return Papyrus::CallStatic("NiOverride", fn, d, static_cast<RE::TESObjectREFR*>(a_actor)); });
            }
        }

        // NG fix (crash 2026-11-12 10:20:52, a Sofia placed after Remove, MuFacialExpressionExtended -> game face
        // morphing on a plain BSTriShape "SofiaHeadHP"): the game's TESNPC::SetRace (1.6.1170 +3B7300) gives an NPC
        // moved to a race other than its originalRace a generated face and puts it in a table (+3C1D50 set / clear,
        // +3C1CD0 lookup) that turns its prebaked FaceGen head off. RaceMenu's ApplyPreset empties originalRace, so
        // the race change back (Remove) counts as a new change: the NPC stays in the table, every head of it is
        // built from its head part meshes, and a mod head mesh without morph shapes crashes face animation. Once it
        // is back in its own race: originalRace emptied and its table entry removed (1.6.1170 only, code checked).
        void ClearRaceChangeFace(RE::TESNPC* a_npc, RE::TESRace* a_race, const std::string& a_name)
        {
            if (!a_npc || !a_race || a_npc->race != a_race) {
                return;
            }
            a_npc->originalRace = nullptr;
            static const auto fns = []() -> std::pair<std::uintptr_t, std::uintptr_t> {
                if (REL::Module::get().version() != REL::Version(1, 6, 1170, 0)) {
                    return { 0, 0 };
                }
                constexpr std::uint8_t setStart[] = { 0x48, 0x85, 0xC9, 0x0F, 0x84 };          // test rcx, rcx / je
                constexpr std::uint8_t getStart[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };    // push rbx / sub rsp, 20
                const auto             set = REL::Offset(0x3C1D50).address();
                const auto             get = REL::Offset(0x3C1CD0).address();
                if (std::memcmp(reinterpret_cast<const void*>(set), setStart, sizeof(setStart)) != 0 ||
                    std::memcmp(reinterpret_cast<const void*>(get), getStart, sizeof(getStart)) != 0) {
                    return { 0, 0 };
                }
                return { set, get };
            }();
            if (!fns.first) {
                logger::info("{} - race-change face not cleared (game is not 1.6.1170 or its code differs)", a_name);
                return;
            }
            using Get = std::uint32_t(RE::TESNPC*);
            using Set = void(RE::TESNPC*, void*, std::uint32_t);
            if (reinterpret_cast<Get*>(fns.second)(a_npc) != 0) {
                reinterpret_cast<Set*>(fns.first)(a_npc, nullptr, 0);  // 0 = remove (as SetRace does)
                logger::info("{} - race-change face removed: its own prebaked face is used again", a_name);
            }
        }

        // NG (user: "Remove should take the Erin preset off a taken-over NPC too"): the Erin race's skin and its
        // per-actor registrations (extra tints, glow head parts, animation) off, then Erin checks the actor again with
        // its own race - an Erin NPC gets its own Erin setup back, another race is dropped. REA (MuErinRace.dll
        // 1.4.28): interface [9] = SkinManager::UnRegisterActorSkin(actor), [5] = re-verify (RemoveErinActorMap +
        // ActorManager unregister + TrackingActors). Main thread.
        // NG fix (user: "make a fix" for the known limit): [5] does not drop the old preset's head-part positions
        // (kept per actor in Erin's actor data, co-saved) nor its tri morph (own map) - an NPC that is an Erin race by
        // its own race kept moved eyes / brows and the preset's face shape. a_oldPreset (the Erin preset that was on
        // it): first a reset preset is loaded through the interface's [7] (load with a part mask) with only head-part
        // positions (4) + tri morph (8): the same head-part slots as the old preset, all neutral (no move / rotation,
        // scale 1), TriMorphType Default (Files::ErinResetPreset). Before [5]: Erin only sets positions on an actor it
        // still counts as Erin (its cached answer, dropped by [5]).
        void ResetErin(RE::Actor* a_actor, const std::string& a_oldPreset)
        {
            if (!g_erin || !a_actor) {
                return;
            }
            auto vtbl = *reinterpret_cast<void***>(g_erin);
            if (const auto reset = Files::ErinResetPreset(a_oldPreset); !reset.empty()) {
                reinterpret_cast<void (*)(void*, RE::Actor*, const char*, std::uint32_t)>(vtbl[7])(g_erin, a_actor, reset.c_str(), 4 | 8);
                Out(std::format("{} - Erin head-part positions + face shape (tri morph) of {} set back to neutral", DisplayName(a_actor), a_oldPreset));
            } else {
                logger::warn("{} - Erin head-part positions / face shape not reset: no head-part slots found for {}", DisplayName(a_actor), a_oldPreset);
            }
            reinterpret_cast<void (*)(void*, RE::Actor*)>(vtbl[9])(g_erin, a_actor);
            reinterpret_cast<void (*)(void*, RE::Actor*)>(vtbl[5])(g_erin, a_actor);
            Out(std::format("{} - Erin preset taken off (Erin skin + extra tints unregistered, checked again)", DisplayName(a_actor)));
        }

        // NG 1.2: a real NPC's own look back (Remove): RaceMenu preset, body morphs and overlays off, its own race,
        // then its 3D rebuilt (Disable / Enable) so its own face shows again. Then (user: "Remove should remove the
        // follower, or move it away, like it already does") it goes back to its own place in the world
        // (MoveToMyEditorLocation) - not disabled, so its own mod keeps working.
        // a_erin: the Erin Preset it had ("" = none) - taken off once its own race is back (ResetErin).
        void RestoreRealLook(RE::Actor* a_actor, RE::FormID a_race, RE::FormID a_skin, RE::FormID a_outfit, std::string a_erin = {})
        {
            auto       npc = a_actor->GetActorBase();
            auto       race = Lookup<RE::TESRace>(a_race);
            auto       chain = Papyrus::Chain::Make();
            const bool shown = a_actor->Is3DLoaded() && !a_actor->IsDisabled();
            g_restoring.insert(a_actor->GetFormID());  // NG: a take-over of it waits until this has finished (Apply)
            // crash fix: its head is taken away first, the head data swapped while it is gone
            if (shown) {
                chain->Then([a_actor](Papyrus::Done d) { return Papyrus::CallMethod(a_actor, "ObjectReference", "Disable", d, false); });
                chain->Then(WaitStep(kSettle));
            }
            if (npc) {
                chain->Then([npc](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "ClearPreset", d, npc); });
            }
            AddRaceMenuCleanup(*chain, a_actor);
            // NG fix (user video 2026-11-12: face still mashed with the preset's after Remove): the preset's sculpt data
            // and custom face morphs stay on the base NPC in RaceMenu (ClearPreset keeps them, no Papyrus function
            // removes them) and reshape every head it builds. Loading an empty preset (flags 0, own race) makes
            // RaceMenu erase both; the empty preset is unmapped again and the NPC's own head data goes back below.
            if (const auto clean = Files::CleanPresetKey(); npc && race && !clean.empty()) {
                chain->Then([a_actor, race, clean](Papyrus::Done d) {
                    return Papyrus::CallStatic("CharGen", "LoadCharacterEx", d, a_actor, race, RE::BSFixedString(clean), std::int32_t(0));
                });
                chain->Then(WaitStep(kSettle));
                chain->Then([npc](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "ClearPreset", d, npc); });
            } else {
                Out(std::format("{} - empty RaceMenu preset not available: the preset's face sculpt / morphs may stay", DisplayName(a_actor)));
            }
            chain->Then([a_actor, race](Papyrus::Done d) { return race && a_actor->GetRace() != race && Papyrus::CallMethod(a_actor, "Actor", "SetRace", d, race); });
            if (npc) {
                chain->Then([npc, race, a_skin, name = DisplayName(a_actor)](Papyrus::Done d) {
                    OnMain([npc, race, a_skin, name, d]() {
                        ClearRaceChangeFace(npc, race, name);
                        RestoreRealHead(npc, a_skin, name);
                        d(Papyrus::Variable{});
                    });
                    return true;
                });
            }
            if (auto outfit = Lookup<RE::BGSOutfit>(a_outfit)) {
                chain->Then([a_actor, outfit](Papyrus::Done d) { return Papyrus::CallMethod(a_actor, "Actor", "SetOutfit", d, outfit, false); });  // its own clothes again
            }
            chain->Then([a_actor](Papyrus::Done d) { return Papyrus::CallMethod(a_actor, "ObjectReference", "MoveToMyEditorLocation", d); });
            if (shown) {
                chain->Then([a_actor](Papyrus::Done d) { return Papyrus::CallMethod(a_actor, "ObjectReference", "Enable", d, false); });
            }
            if (!a_erin.empty() && g_erin) {
                // after its own race is back and (when shown) its 3D is there again
                chain->Then([a_actor, shown](Papyrus::Done d) {
                    if (!shown) {
                        return false;
                    }
                    WaitLoaded(a_actor, 20, d);
                    return true;
                });
                chain->ThenDo([a_actor, a_erin]() { OnMain([a_actor, a_erin]() { ResetErin(a_actor, a_erin); }); });
            }
            chain->ThenDo([name = DisplayName(a_actor), id = a_actor->GetFormID()]() {
                Out(std::format("{} - own look restored (preset cleared, 3D rebuilt), sent back to its own place", name));
                OnMain([id]() { g_restoring.erase(id); });
            });
            chain->Run();
        }

        // The scripts' resetMyData: back to the esp's name / voice / race / style / class, no preset, no
        // spells, RaceMenu preset cleared. (The old script kept the preset race on the actor; we switch back.)
        // NG 1.2: a real NPC entry gets its own name, sex, race and look back (the entry is freed by Dismiss).
        // a_then (a slot only): more chain steps that run after the RaceMenu clean-up and the race change (StoreSlot:
        // move to storage + Disable) - one chain, so no head rebuild runs while the actor is being disabled.
        void ResetSlot(int a_slot, std::function<void(Papyrus::Chain&)> a_then = {})
        {
            auto actor = ActorOf(a_slot);
            RemoveSlotSpells(a_slot);
            RestoreStats(a_slot);
            RE::FormID defRace = 0;
            RE::FormID defSkin = 0;
            RE::FormID defOutfit = 0;
            const bool real = IsReal(a_slot);
            std::string hadErin;  // the Erin preset it had ("" = none)
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                hadErin = s.erinPreset;
                s.name = s.defaultName;
                s.preset.clear();
                s.headMesh.clear();
                s.erinPreset.clear();
                s.race = s.defaultRace;
                s.voice = s.defaultVoice;
                s.style = s.defaultStyle;
                s.npcClass = s.defaultClass;
                s.female = s.defaultFemale;
                s.added = false;  // out of the Followers list
                s.keep = false;
                s.preSummon = false;
                defRace = s.defaultRace;
                defSkin = s.defaultSkin;
                defOutfit = s.defaultOutfit;
            }
            ApplyBase(a_slot);
            if (real) {
                RestoreRealBase(a_slot, true, false);  // skin + head data: RestoreRealLook, while its head is gone
            }
            if (actor && real) {
                if (MuManaged(actor)) {
                    Out(std::format("{} - look left to Mu Dynamic NPC Replacer (not cleared)", DisplayName(actor)));
                } else {
                    RestoreRealLook(actor, defRace, defSkin, defOutfit, hadErin);
                }
                return;
            }
            if (!actor) {
                return;
            }
            auto chain = Papyrus::Chain::Make();
            if (MuManaged(actor)) {
                Out(std::format("{} - look left to Mu Dynamic NPC Replacer (not cleared)", DisplayName(actor)));
            } else {
                if (auto npc = actor->GetActorBase()) {
                    chain->Then([npc](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "ClearPreset", d, npc); });
                }
                AddRaceMenuCleanup(*chain, actor);
                if (auto r = Lookup<RE::TESRace>(defRace)) {
                    // the head rebuild of the SetRace settles before anything else (Disable) touches the actor
                    auto raceSet = std::make_shared<bool>(false);
                    chain->Then([actor, r, raceSet](Papyrus::Done d) {
                        *raceSet = actor->GetRace() != r && Papyrus::CallMethod(actor, "Actor", "SetRace", d, r);
                        return *raceSet;
                    });
                    chain->Then([raceSet](Papyrus::Done d) { return *raceSet && WaitStep(kSettle)(d); });
                }
                if (!hadErin.empty() && g_erin) {
                    chain->ThenDo([actor, hadErin]() { OnMain([actor, hadErin]() { ResetErin(actor, hadErin); }); });  // NG: its Erin preset off too
                }
            }
            if (a_then) {
                a_then(*chain);
            }
            chain->Run();
        }

        // The scripts' summonMe: enable if needed, then move to a_target - here 120 units in front of it
        // (the script used no offset = on top of the player). The log line says where it ended up.
        // a_then (main thread, after the move) replaces the usual lite apply (Apply Settings: the full apply).
        void SummonTo(RE::Actor* a_actor, RE::TESObjectREFR* a_target, std::function<void()> a_then = {})
        {
            if (!a_actor || !a_target) {
                return;
            }
            constexpr float kFront = 120.0f;
            const float     angle = a_target->GetAngleZ();  // radians, 0 = north (+Y), clockwise
            const float     dx = kFront * std::sin(angle);
            const float     dy = kFront * std::cos(angle);
            auto            chain = Papyrus::Chain::Make();
            if (a_actor->IsDisabled()) {
                chain->Then([a_actor](Papyrus::Done d) { return Papyrus::CallMethod(a_actor, "ObjectReference", "Enable", d, false); });
            }
            chain->Then([a_actor, a_target, dx, dy](Papyrus::Done d) {
                return Papyrus::CallMethod(a_actor, "ObjectReference", "MoveTo", d, a_target, dx, dy, 0.0f, true);
            });
            chain->ThenDo([a_actor, a_target, a_then]() {
                OnMain([a_actor, a_target, a_then]() {
                    Out(std::format("Summoned {}: {:.0f} units from {}, disabled {}", DisplayName(a_actor),
                        a_actor->GetPosition().GetDistance(a_target->GetPosition()), DisplayName(a_target), a_actor->IsDisabled()));
                    if (a_then) {
                        a_then();
                        return;
                    }
                    // the script's OnLoad after Enable / MoveTo: lite preset apply (face textures, overlays)
                    if (const int slot = SlotOfActor(a_actor); slot >= 0) {
                        ScheduleUpdate(slot, false, true);
                    }
                });
            });
            chain->Run();
        }

        // The worn armor (every biped slot) and equipped weapons / shield of a_actor, without duplicates
        // (the scripts' getMyGear).
        std::vector<RE::TESBoundObject*> WornGear(RE::Actor* a_actor)
        {
            std::vector<RE::TESBoundObject*> out;
            auto add = [&](RE::TESForm* f) {
                auto b = f ? f->As<RE::TESBoundObject>() : nullptr;
                if (b && std::find(out.begin(), out.end(), b) == out.end()) {
                    out.push_back(b);
                }
            };
            for (std::uint32_t bit = 0; bit < 32; ++bit) {
                add(a_actor->GetWornArmor(static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1u << bit)));
            }
            add(a_actor->GetEquippedObject(true));
            add(a_actor->GetEquippedObject(false));
            return out;
        }

        // ---- lists ----

        void AddChoice(std::vector<Choice>& a_list, RE::FormID a_id, std::string a_name, int a_group = 0)
        {
            if (!a_id || std::any_of(a_list.begin(), a_list.end(), [&](const Choice& c) { return c.id == a_id; })) {
                return;
            }
            a_list.push_back({ a_id, std::move(a_name), a_group });
        }

        // A spell's folder on the Spells page (index into kSpellSchools): the school (skill) of its costliest
        // effect, "Other" for anything else.
        int SpellSchool(RE::SpellItem* a_spell)
        {
            auto eff = a_spell->GetCostliestEffectItem();
            auto mgef = eff ? eff->baseEffect : nullptr;
            switch (mgef ? mgef->GetMagickSkill() : RE::ActorValue::kNone) {
            case RE::ActorValue::kAlteration:
                return 0;
            case RE::ActorValue::kConjuration:
                return 1;
            case RE::ActorValue::kDestruction:
                return 2;
            case RE::ActorValue::kIllusion:
                return 3;
            case RE::ActorValue::kRestoration:
                return 4;
            default:
                return static_cast<int>(kSpellSchools.size()) - 1;
            }
        }

        // A voice type's display name: editor ID (po3 Tweaks) or the form ID.
        std::string VoiceName(const RE::BGSVoiceType* a_v)
        {
            const char* ed = a_v->GetFormEditorID();
            return ed && *ed ? ed : std::format("VoiceType {:08X}", a_v->GetFormID());
        }

        // A race's display name: its name, plus the plugin when two races share a name.
        std::string RaceLabel(const RE::TESRace* a_r)
        {
            std::string name = a_r->GetFullName();
            if (name.empty()) {
                const char* ed = a_r->GetFormEditorID();
                name = ed && *ed ? ed : std::format("Race {:08X}", a_r->GetFormID());
            }
            return name;
        }

        // Builds the drop-down lists (main thread).
        void BuildLists()
        {
            auto l = std::make_shared<Lists>();
            auto dh = RE::TESDataHandler::GetSingleton();

            // races: every playable race of the load order + races met; vampire races are not playable but
            // the old list had them (NordV...), so races already used by followers or met are added too
            std::vector<RE::TESRace*> races;
            if (dh) {
                for (auto r : dh->GetFormArray<RE::TESRace>()) {
                    if (r && r->data.flags.all(RE::RACE_DATA::Flag::kPlayable)) {
                        races.push_back(r);
                    }
                }
            }
            auto vanilla = dh ? dh->LookupForm<RE::BGSListForm>(kRacesList, kPlugin) : nullptr;  // _LazyFollowersRaces
            for (auto f : ListForms(vanilla)) {
                if (auto r = f ? f->As<RE::TESRace>() : nullptr) {
                    races.push_back(r);
                }
            }
            {
                std::lock_guard lock(g_lock);
                for (auto id : g_extraRaces) {
                    if (auto r = Lookup<RE::TESRace>(id)) {
                        races.push_back(r);
                    }
                }
                for (auto& s : g_slots) {
                    if (auto r = Lookup<RE::TESRace>(s.race)) {
                        races.push_back(r);
                    }
                }
            }
            if (auto p = RE::PlayerCharacter::GetSingleton(); p && p->GetRace()) {
                races.push_back(p->GetRace());
            }
            std::unordered_set<RE::TESRace*> seen;
            std::unordered_map<std::string, int> nameCount;
            std::vector<RE::TESRace*> unique;
            for (auto r : races) {
                if (seen.insert(r).second) {
                    unique.push_back(r);
                    ++nameCount[RaceLabel(r)];
                }
            }
            for (auto r : unique) {
                auto label = RaceLabel(r);
                if (nameCount[label] > 1) {
                    const bool vampire = r->HasKeywordString("Vampire");
                    auto       file = r->GetFile(0);
                    label = std::format("{}{} [{}]", label, vampire ? " (Vampire)" : "", file ? file->GetFilename() : "?"sv);
                }
                l->races.push_back({ r->GetFormID(), std::move(label) });
            }
            std::sort(l->races.begin(), l->races.end(), [](const Choice& a, const Choice& b) { return a.name < b.name; });

            // voices: the esp lists with the MCM's names, then voices met
            // Fix (user GIF, 2nd try): the old mod's scripts added voices met to these FormLists, sorted by the
            // NPC's sex (MaleEvenToned, FemaleChild...), and old saves still have them: entries after the esp's
            // own ones must have the right Female flag and not be child voices (they stay in Other Voice)
            auto fitsVoice = [](RE::TESForm* a_f, bool a_female) {
                auto v = a_f ? a_f->As<RE::BGSVoiceType>() : nullptr;
                if (!v || v->data.flags.all(RE::VOICE_TYPE_DATA::Flag::kFemale) != a_female) {
                    return false;
                }
                std::string lower = VoiceName(v);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                return lower.find("child") == std::string::npos;
            };
            auto fv = ListForms(g_femaleVoicesList);
            for (std::size_t i = 0; i < fv.size(); ++i) {
                if (fv[i] && (i < kFemaleVoiceNames.size() || fitsVoice(fv[i], true))) {
                    AddChoice(l->femaleVoices, fv[i]->GetFormID(), i < kFemaleVoiceNames.size() ? kFemaleVoiceNames[i] : VoiceName(fv[i]->As<RE::BGSVoiceType>()));
                }
            }
            auto mv = ListForms(g_maleVoicesList);
            for (std::size_t i = 0; i < mv.size(); ++i) {
                if (mv[i] && (i < kMaleVoiceNames.size() || fitsVoice(mv[i], false))) {
                    AddChoice(l->maleVoices, mv[i]->GetFormID(), i < kMaleVoiceNames.size() ? kMaleVoiceNames[i] : VoiceName(mv[i]->As<RE::BGSVoiceType>()));
                }
            }
            {
                std::lock_guard lock(g_lock);
                // voices met / from the config file: only voices whose Female flag fits the list (fix: older
                // saves put male voices met on female-made slots into Female Voice); child voices not at all
                auto fits = [](const Choice& a_c, bool a_female) {
                    auto v = Lookup<RE::BGSVoiceType>(a_c.id);
                    if (!v || v->data.flags.all(RE::VOICE_TYPE_DATA::Flag::kFemale) != a_female) {
                        return false;
                    }
                    std::string lower = a_c.name;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    return lower.find("child") == std::string::npos;
                };
                for (auto& c : g_extraFemaleVoices) {
                    if (fits(c, true)) {
                        AddChoice(l->femaleVoices, c.id, c.name);
                    }
                }
                for (auto& c : g_extraMaleVoices) {
                    if (fits(c, false)) {
                        AddChoice(l->maleVoices, c.id, c.name);
                    }
                }
                // a follower's current voice is always pickable
                for (auto& s : g_slots) {
                    if (auto v = Lookup<RE::BGSVoiceType>(s.voice)) {
                        const bool female = v->data.flags.all(RE::VOICE_TYPE_DATA::Flag::kFemale);
                        AddChoice(female ? l->femaleVoices : l->maleVoices, v->GetFormID(), VoiceName(v));
                    }
                }
            }
            // NG: Other Voice = every other voice type of the load order (modded / unlisted voices), sorted
            if (dh) {
                std::unordered_set<RE::FormID> listed;
                for (auto* list : { &l->femaleVoices, &l->maleVoices }) {
                    for (auto& c : *list) {
                        listed.insert(c.id);
                    }
                }
                for (auto v : dh->GetFormArray<RE::BGSVoiceType>()) {
                    if (v && !listed.contains(v->GetFormID())) {
                        auto file = v->GetFile(0);
                        AddChoice(l->otherVoices, v->GetFormID(), std::format("{} [{}]", VoiceName(v), file ? file->GetFilename() : "?"sv));
                    }
                }
                std::sort(l->otherVoices.begin(), l->otherVoices.end(), [](const Choice& a, const Choice& b) { return a.name < b.name; });
                // NG (user): the Override box only for a modded follower's voice - a modded voice (not a base-game
                // file) that a unique NPC of another plugin uses (what FindVoiceOwner looks for on Apply)
                for (auto npc : dh->GetFormArray<RE::TESNPC>()) {
                    auto v = npc ? npc->voiceType : nullptr;
                    auto file = npc ? npc->GetFile(0) : nullptr;
                    if (v && npc->IsUnique() && !(file && file->GetFilename() == kPlugin) && !l->ownedVoices.contains(v->GetFormID()) && Voice::IsModded(v)) {
                        l->ownedVoices.insert(v->GetFormID());
                    }
                }
                // NG (user): also voices a unique NPC has in its plugin file but not in the game (another mod changed it,
                // e.g. Severa) - Voice's background plugin scan, the same file data Apply uses to find the NPC
                for (auto id : Voice::FileOwnedVoices()) {
                    l->ownedVoices.insert(id);
                }
                // NG fix (user: "Why does JJSofiaVoiceType go to the Female tab?" - a nearby Sofia / a follower using it
                // put it in Female Voice, so Other Voice no longer had it): a modded follower's voice is only listed in
                // Other Voice (the LazyFollowers.esp lists never hold one)
                const auto espF = ListForms(g_femaleVoicesList);
                const auto espM = ListForms(g_maleVoicesList);
                bool moved = false;
                for (auto* list : { &l->femaleVoices, &l->maleVoices }) {
                    const auto& espList = list == &l->femaleVoices ? espF : espM;
                    for (auto it = list->begin(); it != list->end();) {
                        auto v = Lookup<RE::BGSVoiceType>(it->id);
                        const bool inEsp = v && std::find(espList.begin(), espList.end(), v) != espList.end();
                        if (v && !inEsp && l->ownedVoices.contains(it->id)) {
                            auto file = v->GetFile(0);
                            AddChoice(l->otherVoices, v->GetFormID(), std::format("{} [{}]", VoiceName(v), file ? file->GetFilename() : "?"sv));
                            it = list->erase(it);
                            moved = true;
                        } else {
                            ++it;
                        }
                    }
                }
                if (moved) {
                    std::sort(l->otherVoices.begin(), l->otherVoices.end(), [](const Choice& a, const Choice& b) { return a.name < b.name; });
                }
            }

            auto cl = ListForms(g_classesList);
            for (std::size_t i = 0; i < cl.size(); ++i) {
                if (cl[i]) {
                    AddChoice(l->classes, cl[i]->GetFormID(), i < kClassNames.size() ? kClassNames[i] : std::string(cl[i]->GetName()));
                }
            }

            // spells: the esp list, + every playable spell of the load order after "Load all spells"
            auto spellLabel = [](RE::SpellItem* sp) {
                std::string n = sp->GetFullName();
                if (auto slot = sp->GetEquipSlot()) {
                    switch (slot->GetFormID()) {
                    case 0x13F44:
                        n += "(-)";  // either hand
                        break;
                    case 0x13F42:
                        n += "(R)";
                        break;
                    case 0x13F43:
                        n += "(L)";
                        break;
                    default:
                        break;
                    }
                }
                return n;
            };
            for (auto f : ListForms(g_spellsList)) {
                if (auto sp = f ? f->As<RE::SpellItem>() : nullptr) {
                    AddChoice(l->spells, sp->GetFormID(), spellLabel(sp), SpellSchool(sp));
                }
            }
            if (g_allSpells && dh) {
                std::vector<Choice> extra;
                for (auto sp : dh->GetFormArray<RE::SpellItem>()) {
                    if (sp && sp->GetSpellType() == RE::MagicSystem::SpellType::kSpell && sp->GetFullName()[0] != '\0' && sp->GetPlayable()) {
                        extra.push_back({ sp->GetFormID(), spellLabel(sp), SpellSchool(sp) });
                    }
                }
                std::sort(extra.begin(), extra.end(), [](const Choice& a, const Choice& b) { return a.name < b.name; });
                for (auto& c : extra) {
                    AddChoice(l->spells, c.id, c.name, c.group);
                }
            }

            l->presets = Files::ListPresets();
            if (g_erin) {
                l->erinPresets = Files::ListErinPresets();  // NG (user): Erin Preset row (only with the Erin race)
                // NG (user): ... shown only while an Erin race is picked - the races of ErinRace.esm (or named ErinRace...)
                if (auto dh = RE::TESDataHandler::GetSingleton()) {
                    for (auto r : dh->GetFormArray<RE::TESRace>()) {
                        if (!r) {
                            continue;
                        }
                        const auto file = r->GetFile(0);
                        const char* ed = r->GetFormEditorID();
                        if ((file && _stricmp(file->fileName, "ErinRace.esm") == 0) || (ed && _strnicmp(ed, "ErinRace", 8) == 0)) {
                            l->erinRaces.insert(r->GetFormID());
                        }
                    }
                }
            }
            // NG (user): each preset's race (picked by itself in Appearance, shown first in Race); a race not listed yet
            // (not playable, not met) is added to the Race list
            bool racesAdded = false;
            for (const auto& p : l->presets) {
                if (const auto id = Files::PresetRace(p)) {
                    l->presetRaces.emplace(p, id);
                    if (std::none_of(l->races.begin(), l->races.end(), [&](const Choice& c) { return c.id == id; })) {
                        if (auto r = Lookup<RE::TESRace>(id)) {
                            l->races.push_back({ id, RaceLabel(r) });
                            racesAdded = true;
                        }
                    }
                }
            }
            if (racesAdded) {
                std::sort(l->races.begin(), l->races.end(), [](const Choice& a, const Choice& b) { return a.name < b.name; });
            }
            l->hiddenPresets = Files::ListHiddenPresets();
            l->npcPresets = Files::ListNPCPresets();

            std::lock_guard lock(g_lock);
            g_lists = std::move(l);
        }

        std::shared_ptr<const Lists> CurrentLists()
        {
            std::lock_guard lock(g_lock);
            return g_lists;
        }

        // The MCM's getCustomRace / getCustomVoiceParser: remember the race and voice of an NPC we met.
        void Meet(RE::Actor* a_actor)
        {
            if (!a_actor) {
                return;
            }
            std::lock_guard lock(g_lock);
            if (auto r = a_actor->GetRace(); r && !r->data.flags.all(RE::RACE_DATA::Flag::kPlayable)) {
                if (std::find(g_extraRaces.begin(), g_extraRaces.end(), r->GetFormID()) == g_extraRaces.end()) {
                    g_extraRaces.push_back(r->GetFormID());
                    Out(std::format("Added custom race {}", RaceLabel(r)));
                }
            }
            auto npc = a_actor->GetActorBase();
            auto v = npc ? npc->voiceType : nullptr;
            // Fix (user GIF): sorted by the voice's own Female flag, not the NPC's sex (Lazy Followers' slots
            // change sex, so a male voice landed in Female Voice); children's voices and our own followers'
            // voices are not added (they stay in Other Voice)
            if (v && !a_actor->IsChild() && SlotOfActor(a_actor) < 0) {
                const bool female = v->data.flags.all(RE::VOICE_TYPE_DATA::Flag::kFemale);
                auto&      list = female ? g_extraFemaleVoices : g_extraMaleVoices;
                auto       esp = ListForms(female ? g_femaleVoicesList : g_maleVoicesList);
                const bool inEsp = std::find(esp.begin(), esp.end(), v) != esp.end();
                if (!inEsp && std::none_of(list.begin(), list.end(), [&](const Choice& c) { return c.id == v->GetFormID(); })) {
                    list.push_back({ v->GetFormID(), VoiceName(v) });
                    Out(std::format("Added custom voice {} from {}", VoiceName(v), DisplayName(a_actor)));
                }
            }
        }

        // ---- snapshot ----

        // The Followers page's Distance / Location (NG 1.1, user): distance only when the follower is in your
        // interior cell or your worldspace; location = "<worldspace>, <location>" (e.g. "Skyrim, Whiterun"),
        // inside: the location, else the cell's name.
        void FillWhere(RE::Actor* a_actor, Snapshot& a_snap)
        {
            auto player = RE::PlayerCharacter::GetSingleton();
            auto cell = a_actor->GetParentCell();
            auto pcell = player ? player->GetParentCell() : nullptr;
            if (cell && pcell) {
                const bool same = cell->IsInteriorCell() ? cell == pcell : (!pcell->IsInteriorCell() && a_actor->GetWorldspace() == player->GetWorldspace());
                if (same) {
                    a_snap.selDistance = player->GetPosition().GetDistance(a_actor->GetPosition()) / 70.0f;  // units per metre (Nearby Actors)
                }
            }
            std::vector<std::string> parts;
            auto add = [&](const char* a_name) {
                if (a_name && *a_name && std::find(parts.begin(), parts.end(), a_name) == parts.end()) {
                    parts.emplace_back(a_name);
                }
            };
            if (auto ws = a_actor->GetWorldspace(); ws && cell && !cell->IsInteriorCell()) {
                add(ws->GetFullName());
            }
            if (auto loc = a_actor->GetCurrentLocation()) {
                add(loc->GetFullName());
            }
            if (parts.empty() && cell) {
                add(cell->GetFullName());
            }
            for (std::size_t i = 0; i < parts.size(); ++i) {
                a_snap.selLocation += (i ? ", " : "") + parts[i];
            }
        }

        void BuildSnapshot()
        {
            Snapshot s;
            s.valid = g_formsFound && g_inGame.load();
            static bool raceMenu = false;  // NG perf: looked up until found, then kept
            if (!raceMenu) {
                raceMenu = Papyrus::HasFunction("CharGen", "SaveCharacterPreset");
            }
            s.raceMenu = raceMenu;
            s.fgt = g_fgtQuest != nullptr;
            s.consoleRef = IdOf(ConsoleActor());
            s.consoleBusy = ConsoleHasSelection();
            s.crosshairRef = IdOf(CrosshairActor());
            {
                std::lock_guard lock(g_lock);
                s.slots = g_slots;
                s.styleNames = g_styleNames;
                s.lists = g_lists;
            }
            for (int i = 0; i < static_cast<int>(s.slots.size()); ++i) {
                if (s.slots[i].real && s.slots[i].ref) {
                    s.slots[i].moddedVoice = Voice::IsModded(Lookup<RE::BGSVoiceType>(s.slots[i].defaultVoice));
                }
            }
            s.live.resize(g_actors.size());
            for (std::size_t i = 0; i < g_actors.size(); ++i) {
                if (auto a = ActorOf(static_cast<int>(i))) {
                    s.live[i].enabled = !a->IsDisabled();
                    s.live[i].loaded = a->Is3DLoaded();
                    s.live[i].following = Following(a);
                }
            }
            if (auto a = ActorOf(g_snapSelected.load())) {
                FillWhere(a, s);
                auto av = a->AsActorValueOwner();
                for (int i = 0; i < kStatCount; ++i) {
                    s.stats[i] = av->GetActorValue(kStats[i].second);
                }
                for (int i = 0; i < kSkillCount; ++i) {
                    s.skills[i] = av->GetActorValue(kSkills[i].second);
                }
            }
            auto            shared = std::make_shared<const Snapshot>(std::move(s));
            std::lock_guard lock(g_snapLock);
            g_snap = std::move(shared);
        }

        // ---- co-save ----
        constexpr std::uint32_t kCosaveID = 'LFNG';
        constexpr std::uint32_t kRecSettings = 'SETS';
        constexpr std::uint32_t kRecStyles = 'STYL';
        constexpr std::uint32_t kRecMet = 'MEET';
        constexpr std::uint32_t kRecSlot = 'SLOT';
        constexpr std::uint32_t kRecSex = 'SEXF';  // NG 1.1: per active slot its ref + sex (1 = female)
        constexpr std::uint32_t kRecAdded = 'ADDF';  // NG 1.1: ref of a slot in the Followers list without a preset
        constexpr std::uint32_t kRecStats = 'STAT';  // NG 1.1: ref + Stats page values before they were changed
        constexpr std::uint32_t kRecKeep = 'KEEP';   // NG (user): ref of a slot with Keep in New Game ticked
        constexpr std::uint32_t kRecPreSummon = 'PSUM';  // NG (user): ref of a slot with Pre Summon ticked
        constexpr std::uint32_t kRecErin = 'ERIN';       // NG (user): ref + Erin Preset file of an active slot
        constexpr std::uint32_t kRecRealText = 'RTXT';  // NG: a real NPC's own reference name data (after its REAL)
        constexpr std::uint32_t kRecReal = 'REAL';   // NG 1.2: a real NPC entry: ref, base, its own name / race / sex (before SLOT...)
        constexpr std::uint32_t kVersion = 2;     // 2 = written with the ESL-flagged esp; 1 = old esp IDs (mapped on load)
        std::uint32_t           g_loadVersion = kVersion;  // version of the co-save record being read

        void WriteString(SKSE::SerializationInterface* a_intfc, const std::string& a_s)
        {
            const auto n = static_cast<std::uint32_t>(a_s.size());
            a_intfc->WriteRecordData(n);
            if (n) {
                a_intfc->WriteRecordData(a_s.data(), n);
            }
        }

        bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& a_s)
        {
            std::uint32_t n = 0;
            if (!a_intfc->ReadRecordData(n) || n > 4096) {
                return false;
            }
            a_s.resize(n);
            return n == 0 || a_intfc->ReadRecordData(a_s.data(), n) == n;
        }

        void WriteForm(SKSE::SerializationInterface* a_intfc, RE::FormID a_id) { a_intfc->WriteRecordData(a_id); }

        // Reads a form ID and maps it to this load order (0 if its plugin is gone). Version 1 records were
        // written with the non-ESL esp: an esp form there no longer resolves (the esp is light now), so its
        // old local ID is translated with EslIds.h.
        RE::FormID ReadForm(SKSE::SerializationInterface* a_intfc)
        {
            RE::FormID old = 0, now = 0;
            a_intfc->ReadRecordData(old);
            if (old && a_intfc->ResolveFormID(old, now)) {
                return now;
            }
            if (old && g_loadVersion < 2 && (old >> 24) < 0xFE) {
                if (const auto local = EslId(old)) {
                    auto dh = RE::TESDataHandler::GetSingleton();
                    if (auto f = dh ? dh->LookupForm(local, kPlugin) : nullptr) {
                        return f->GetFormID();
                    }
                }
            }
            return 0;
        }

        void OnSave(SKSE::SerializationInterface* a_intfc)
        {
            std::lock_guard lock(g_lock);
            if (a_intfc->OpenRecord(kRecSettings, kVersion)) {
                a_intfc->WriteRecordData(std::uint8_t(g_stylesEnabled));
                a_intfc->WriteRecordData(std::uint8_t(g_classesEnabled));
                a_intfc->WriteRecordData(std::uint8_t(g_autoEquip));
                a_intfc->WriteRecordData(std::uint8_t(g_allSpells));
            }
            if (a_intfc->OpenRecord(kRecStyles, kVersion)) {
                for (int i = 0; i < kStyles; ++i) {
                    WriteString(a_intfc, g_styleNames[i]);
                    const auto v = ReadStyle(g_styles[i]);
                    a_intfc->WriteRecordData(v.data(), static_cast<std::uint32_t>(sizeof(float) * kStyleValues));
                }
            }
            if (a_intfc->OpenRecord(kRecMet, kVersion)) {
                for (auto* list : { &g_extraFemaleVoices, &g_extraMaleVoices }) {
                    a_intfc->WriteRecordData(static_cast<std::uint32_t>(list->size()));
                    for (auto& c : *list) {
                        WriteForm(a_intfc, c.id);
                        WriteString(a_intfc, c.name);
                    }
                }
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(g_extraRaces.size()));
                for (auto id : g_extraRaces) {
                    WriteForm(a_intfc, id);
                }
            }
            // real NPC entries first: loading puts them into free entries, then SLOT / SEXF / ADDF find them by ref
            for (const auto& s : g_slots) {
                if (s.real && s.ref && a_intfc->OpenRecord(kRecReal, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                    WriteForm(a_intfc, s.base);
                    WriteString(a_intfc, s.defaultName);
                    WriteForm(a_intfc, s.defaultRace);
                    a_intfc->WriteRecordData(std::uint8_t(s.defaultFemale));
                }
                if (s.real && s.ref && s.textSaved && a_intfc->OpenRecord(kRecRealText, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                    a_intfc->WriteRecordData(std::uint8_t(s.textHad));
                    WriteString(a_intfc, s.textName);
                    WriteForm(a_intfc, s.textMessage);
                    WriteForm(a_intfc, s.textQuest);
                    a_intfc->WriteRecordData(s.textInstance);
                    a_intfc->WriteRecordData(s.textTemper);
                }
            }
            for (std::size_t i = 0; i < g_slots.size(); ++i) {
                const auto& s = g_slots[i];
                if (!s.Active() || !a_intfc->OpenRecord(kRecSlot, kVersion)) {
                    continue;
                }
                WriteForm(a_intfc, s.ref);
                WriteString(a_intfc, s.name);
                WriteString(a_intfc, s.preset);
                WriteString(a_intfc, s.headMesh);
                WriteForm(a_intfc, s.race);
                WriteForm(a_intfc, s.voice);
                WriteForm(a_intfc, s.style);
                WriteForm(a_intfc, s.npcClass);
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.spells.size()));
                for (auto id : s.spells) {
                    WriteForm(a_intfc, id);
                }
            }
            for (const auto& s : g_slots) {
                if (s.Active() && a_intfc->OpenRecord(kRecSex, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                    a_intfc->WriteRecordData(std::uint8_t(s.female));
                }
                if (!s.Active() && s.added && a_intfc->OpenRecord(kRecAdded, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                }
                if (s.keep && s.ref && a_intfc->OpenRecord(kRecKeep, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                }
                if (s.preSummon && s.ref && a_intfc->OpenRecord(kRecPreSummon, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                }
                if (s.Active() && s.ref && !s.erinPreset.empty() && a_intfc->OpenRecord(kRecErin, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                    WriteString(a_intfc, s.erinPreset);
                }
                if (!s.statDefaults.empty() && a_intfc->OpenRecord(kRecStats, kVersion)) {
                    WriteForm(a_intfc, s.ref);
                    a_intfc->WriteRecordData(static_cast<std::uint32_t>(s.statDefaults.size()));
                    for (auto& [name, value] : s.statDefaults) {
                        WriteString(a_intfc, name);
                        a_intfc->WriteRecordData(value);
                    }
                }
            }
        }

        // Back to the esp's state for every slot and style (a new game, or before a save's data is read).
        void ResetAll()
        {
            std::lock_guard lock(g_lock);
            for (std::size_t i = 0; i < g_slots.size(); ++i) {
                auto& s = g_slots[i];
                if (s.real) {
                    s = Slot{};  // NG 1.2: real NPC entries are freed (the save's REAL records fill them again)
                    s.real = true;
                    continue;
                }
                s.name = s.defaultName;
                s.preset.clear();
                s.headMesh.clear();
                s.erinPreset.clear();
                s.race = s.defaultRace;
                s.voice = s.defaultVoice;
                s.style = s.defaultStyle;
                s.npcClass = s.defaultClass;
                s.female = s.defaultFemale;
                s.added = false;
                s.keep = false;
                s.preSummon = false;
                s.statDefaults.clear();
                s.spells.clear();
            }
            for (int i = 0; i < kStyles; ++i) {
                g_styleNames[i] = kStyleDefaultNames[i];
                WriteStyle(g_styles[i], g_styleDefaults[i]);
            }
            g_extraFemaleVoices.clear();
            g_extraMaleVoices.clear();
            g_extraRaces.clear();
            g_stylesEnabled = true;
            g_classesEnabled = true;
            g_autoEquip = false;
            g_allSpells = false;
        }

        // ---- Settings page file (LazyFollowersNGSettings.json) ----
        // The old mod kept these in the save (esp globals + MCM script); the user wants them like other SMF
        // mods: one file, the same in every save and a new game. Applied after every load / new game.
        // nullptr = not in the file (NG, user: Apply Delay removed; an old "ApplyDelay" entry is ignored)
        constexpr std::array<const char*, static_cast<int>(Global::kCount)> kGlobalKeys{ nullptr, "RMFullMode", "RMLiteMode",
            "LoggingMode", "Sandboxing" };

        // Current Settings page values -> the file.
        void SaveSettings()
        {
            nlohmann::json j = nlohmann::json::object();
            for (int i = 0; i < static_cast<int>(Global::kCount); ++i) {
                if (kGlobalKeys[i]) {
                    j[kGlobalKeys[i]] = GetGlobal(static_cast<Global>(i));
                }
            }
            {
                std::lock_guard lock(g_lock);
                j["CustomCombatStyles"] = g_stylesEnabled;
                j["CustomClasses"] = g_classesEnabled;
                j["AutoEquip"] = g_autoEquip;
                j["IncludeEquipment"] = g_includeEquip;
                j["CustomSpells"] = g_spellsEnabled;
                j["ScanRadius"] = g_scanRadius;
                j["NearbyRows"] = g_nearbyRows;
                j["NearbyWidths"] = g_nearbyWidths;
                j["PresetRows"] = g_presetRows;
                j["PresetWidths"] = g_presetWidths;
            }
            const auto key = OpenWindowKey();
            j["OpenWindowKey"] = { { "VirtualKey", key.vk }, { "Mods", key.mods } };
            const auto summon = SummonKey();
            j["SummonKey"] = { { "VirtualKey", summon.vk }, { "Mods", summon.mods } };
            const auto summonAll = SummonAllKey();
            j["SummonAllKey"] = { { "VirtualKey", summonAll.vk }, { "Mods", summonAll.mods } };
            const auto savePreset = SavePresetKey();
            j["SavePresetKey"] = { { "VirtualKey", savePreset.vk }, { "Mods", savePreset.mods } };
            const auto openInv = OpenInventoryKey();
            j["OpenInventoryKey"] = { { "VirtualKey", openInv.vk }, { "Mods", openInv.mods } };
            const auto nearbyWin = NearbyWindowKey();
            j["NearbyWindowKey"] = { { "VirtualKey", nearbyWin.vk }, { "Mods", nearbyWin.mods } };
            Files::WriteJson(Files::SettingsFile(), j);
        }

        // The settings that need no game: Open Window hotkey and the Nearby Actors column widths.
        void LoadUiSettings(const nlohmann::json& a_j)
        {
            for (auto [name, store] : { std::pair{ "OpenWindowKey", &g_openKey }, std::pair{ "SummonKey", &g_summonKey }, std::pair{ "SummonAllKey", &g_summonAllKey },
                                         std::pair{ "SavePresetKey", &g_savePresetKey }, std::pair{ "OpenInventoryKey", &g_openInvKey },
                                         std::pair{ "NearbyWindowKey", &g_nearbyWinKey } }) {
                if (auto v = Files::Find(a_j, name); v && v->is_object()) {
                    KeyBinding k;
                    if (auto vk = Files::Find(*v, "VirtualKey"); vk && vk->is_number_unsigned()) {
                        k.vk = std::min<std::uint32_t>(vk->get<std::uint32_t>(), 255);
                    }
                    if (auto m = Files::Find(*v, "Mods"); m && m->is_number_unsigned()) {
                        k.mods = m->get<std::uint32_t>() & (kModCtrl | kModShift | kModAlt);
                    }
                    *store = static_cast<std::uint64_t>(k.vk) | (static_cast<std::uint64_t>(k.mods) << 32);
                }
            }
            for (auto [name, store] : { std::pair{ "NearbyWidths", &g_nearbyWidths }, std::pair{ "PresetWidths", &g_presetWidths } }) {
                if (auto v = Files::Find(a_j, name); v && v->is_array()) {
                    std::vector<float> w;
                    for (auto& x : *v) {
                        if (x.is_number()) {
                            w.push_back(std::clamp(x.get<float>(), 20.0f, 2000.0f));
                        }
                    }
                    std::lock_guard lock(g_lock);
                    *store = std::move(w);
                }
            }
            if (auto v = Files::Find(a_j, "PresetRows"); v && v->is_number_integer()) {
                std::lock_guard lock(g_lock);
                g_presetRows = std::clamp(v->get<int>(), 1, 50);
            }
        }

        // The file -> globals + flags (over what the save had). No file yet = the save's values stay.
        void LoadSettings()
        {
            auto j = Files::ReadJson(Files::SettingsFile());
            if (!j) {
                return;
            }
            for (int i = 0; i < static_cast<int>(Global::kCount); ++i) {
                if (auto v = kGlobalKeys[i] ? Files::Find(*j, kGlobalKeys[i]) : nullptr; v && v->is_number()) {
                    if (auto g = g_globals[i]) {
                        g->value = static_cast<float>(v->get<int>());
                    }
                }
            }
            auto flag = [&](const char* a_key, bool& a_out) {
                if (auto v = Files::Find(*j, a_key); v && v->is_boolean()) {
                    a_out = v->get<bool>();
                }
            };
            std::lock_guard lock(g_lock);
            flag("CustomCombatStyles", g_stylesEnabled);
            flag("CustomClasses", g_classesEnabled);
            flag("AutoEquip", g_autoEquip);
            flag("IncludeEquipment", g_includeEquip);
            flag("CustomSpells", g_spellsEnabled);
            if (auto v = Files::Find(*j, "ScanRadius"); v && v->is_number()) {
                g_scanRadius = std::clamp(v->get<float>(), 1.0f, 100.0f);
            }
            if (auto v = Files::Find(*j, "NearbyRows"); v && v->is_number_integer()) {
                g_nearbyRows = std::clamp(v->get<int>(), 1, 50);
            }
            LoadUiSettings(*j);
        }

        void OnRevert(SKSE::SerializationInterface*)
        {
            ++g_generation;
            for (int i = g_lfCount; i < static_cast<int>(g_slots.size()); ++i) {
                RestoreRealBase(i, false, true);  // NG 1.2: real NPCs' base name / sex (+ head data if not shown) back before their entries are freed
            }
            ResetAll();
            for (std::size_t i = 0; i < g_slots.size(); ++i) {
                ApplyBase(static_cast<int>(i), false);  // base forms back to the esp values (they stay in memory between loads)
            }
        }

        void OnLoad(SKSE::SerializationInterface* a_intfc)
        {
            std::uint32_t type = 0, version = 0, length = 0;
            std::lock_guard lock(g_lock);
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                g_loadVersion = version;
                if (version != kVersion && version != 1) {
                    logger::warn("Co-save record {:08X} has version {}, skipped", type, version);
                    continue;
                }
                if (type == kRecSettings) {
                    std::uint8_t a = 1, b = 1, c = 0, d = 0;
                    a_intfc->ReadRecordData(a);
                    a_intfc->ReadRecordData(b);
                    a_intfc->ReadRecordData(c);
                    a_intfc->ReadRecordData(d);
                    g_stylesEnabled = a != 0;
                    g_classesEnabled = b != 0;
                    g_autoEquip = c != 0;
                    g_allSpells = d != 0;
                } else if (type == kRecStyles) {
                    for (int i = 0; i < kStyles; ++i) {
                        std::string                     name;
                        std::array<float, kStyleValues> v{};
                        if (!ReadString(a_intfc, name) || a_intfc->ReadRecordData(v.data(), sizeof(float) * kStyleValues) != sizeof(float) * kStyleValues) {
                            break;
                        }
                        g_styleNames[i] = name;
                        WriteStyle(g_styles[i], v);
                    }
                } else if (type == kRecMet) {
                    for (auto* list : { &g_extraFemaleVoices, &g_extraMaleVoices }) {
                        std::uint32_t n = 0;
                        a_intfc->ReadRecordData(n);
                        for (std::uint32_t k = 0; k < n && k < 1000; ++k) {
                            Choice c;
                            c.id = ReadForm(a_intfc);
                            ReadString(a_intfc, c.name);
                            if (c.id) {
                                list->push_back(std::move(c));
                            }
                        }
                    }
                    std::uint32_t n = 0;
                    a_intfc->ReadRecordData(n);
                    for (std::uint32_t k = 0; k < n && k < 1000; ++k) {
                        if (auto id = ReadForm(a_intfc)) {
                            g_extraRaces.push_back(id);
                        }
                    }
                } else if (type == kRecReal) {
                    Slot in;
                    in.real = true;
                    in.ref = ReadForm(a_intfc);
                    in.base = ReadForm(a_intfc);
                    ReadString(a_intfc, in.defaultName);
                    in.defaultRace = ReadForm(a_intfc);
                    std::uint8_t female = 0;
                    a_intfc->ReadRecordData(female);
                    in.defaultFemale = female != 0;
                    in.female = in.defaultFemale;
                    in.name = in.defaultName;
                    in.race = in.defaultRace;
                    if (auto a = Lookup<RE::Actor>(in.ref); a && a->GetActorBase()) {
                        OwnDefaults(a->GetActorBase(), in);  // its plugin's skin / outfit / voice (kDataLoaded snapshot)
                        in.voice = in.defaultVoice;
                    }
                    auto free = std::find_if(g_slots.begin() + g_lfCount, g_slots.end(), [](const Slot& s) { return s.real && !s.ref; });
                    if (!in.ref || !Lookup<RE::Actor>(in.ref)) {
                        logger::warn("Co-save: real NPC follower {:08X} is not in the game any more, skipped", in.ref);
                    } else if (std::any_of(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == in.ref; }) || free == g_slots.end()) {
                        logger::warn("Co-save: real NPC follower {:08X} already listed or no free entry, skipped", in.ref);
                    } else {
                        *free = std::move(in);
                    }
                } else if (type == kRecRealText) {
                    const auto   ref = ReadForm(a_intfc);
                    std::uint8_t had = 0;
                    std::string  name;
                    a_intfc->ReadRecordData(had);
                    ReadString(a_intfc, name);
                    const auto   message = ReadForm(a_intfc);
                    const auto   quest = ReadForm(a_intfc);
                    std::int32_t instance = -1;
                    float        temper = 1.0f;
                    a_intfc->ReadRecordData(instance);
                    a_intfc->ReadRecordData(temper);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.real && s.ref && s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->textSaved = true;
                        it->textHad = had != 0;
                        it->textName = std::move(name);
                        it->textMessage = message;
                        it->textQuest = quest;
                        it->textInstance = instance;
                        it->textTemper = temper;
                    }
                } else if (type == kRecSlot) {
                    const auto ref = ReadForm(a_intfc);
                    Slot       in;
                    ReadString(a_intfc, in.name);
                    ReadString(a_intfc, in.preset);
                    in.preset = PresetKey(std::move(in.preset));
                    ReadString(a_intfc, in.headMesh);
                    in.race = ReadForm(a_intfc);
                    in.voice = ReadForm(a_intfc);
                    in.style = ReadForm(a_intfc);
                    in.npcClass = ReadForm(a_intfc);
                    std::uint32_t n = 0;
                    a_intfc->ReadRecordData(n);
                    for (std::uint32_t k = 0; k < n && k < 1000; ++k) {
                        if (auto id = ReadForm(a_intfc)) {
                            in.spells.push_back(id);
                        }
                    }
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it == g_slots.end()) {
                        logger::warn("Co-save: follower {:08X} is not a slot any more, skipped", ref);
                        continue;
                    }
                    it->name = in.name;
                    it->preset = in.preset;
                    it->headMesh = in.headMesh;
                    it->race = in.race ? in.race : it->defaultRace;
                    it->voice = in.voice ? in.voice : it->defaultVoice;
                    it->style = in.style ? in.style : it->defaultStyle;
                    it->npcClass = in.npcClass ? in.npcClass : it->defaultClass;
                    it->spells = std::move(in.spells);
                } else if (type == kRecSex) {
                    const auto   ref = ReadForm(a_intfc);
                    std::uint8_t female = 0;
                    a_intfc->ReadRecordData(female);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->female = female != 0;
                    }
                } else if (type == kRecAdded) {
                    const auto ref = ReadForm(a_intfc);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->added = true;
                    }
                } else if (type == kRecKeep) {
                    const auto ref = ReadForm(a_intfc);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->keep = true;
                    }
                } else if (type == kRecPreSummon) {
                    const auto ref = ReadForm(a_intfc);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->preSummon = true;
                    }
                } else if (type == kRecErin) {
                    const auto  ref = ReadForm(a_intfc);
                    std::string file;
                    ReadString(a_intfc, file);
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref && s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->erinPreset = std::move(file);
                    }
                } else if (type == kRecStats) {
                    const auto                   ref = ReadForm(a_intfc);
                    std::uint32_t                n = 0;
                    std::map<std::string, float> d;
                    a_intfc->ReadRecordData(n);
                    for (std::uint32_t k = 0; k < n && k < 100; ++k) {
                        std::string name;
                        float       value = 0.0f;
                        if (!ReadString(a_intfc, name) || !a_intfc->ReadRecordData(value)) {
                            break;
                        }
                        d[name] = value;
                    }
                    auto it = std::find_if(g_slots.begin(), g_slots.end(), [&](const Slot& s) { return s.ref == ref; });
                    if (it != g_slots.end()) {
                        it->statDefaults = std::move(d);
                    }
                } else {
                    logger::warn("Unknown co-save record {:08X}", type);
                }
            }
            // sex on the base NPCs now, before the followers' 3D is built (the rest follows in OnGameLoaded)
            for (std::size_t i = 0; i < g_slots.size(); ++i) {
                ApplySex(static_cast<int>(i));
                ApplyRealSkin(static_cast<int>(i));  // NG 1.2: before the 3D is built too
            }
        }

        // ---- events ----

        // Crosshair event sink: the reference under the crosshair (none when it leaves) + the last actor looked at.
        class CrosshairSink : public RE::BSTEventSink<SKSE::CrosshairRefEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::CrosshairRefEvent* a_event, RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override
            {
                std::lock_guard lock(g_crossLock);
                g_crosshair = a_event && a_event->crosshairRef ? a_event->crosshairRef->GetHandle() : RE::ObjectRefHandle{};
                if (auto actor = a_event && a_event->crosshairRef ? a_event->crosshairRef->As<RE::Actor>() : nullptr; actor && !actor->IsPlayerRef()) {
                    g_lastCrossActor = actor->GetHandle();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // NG (user): Nearby Actors scans by itself after a save loads and when you enter a new cell, kScanDelay
        // seconds later (the NPCs there need to load first). Several requests in a row (walking through
        // exterior cells) end in one scan.
        constexpr float           kScanDelay = 2.0f;
        std::atomic<unsigned>     g_autoScanTicket{ 0 };

        void AutoScanSoon()
        {
            const unsigned ticket = ++g_autoScanTicket;
            After(kScanDelay, [ticket]() {
                if (ticket == g_autoScanTicket.load()) {
                    ScanNearby();
                }
            });
        }

        // The player entered a cell (PlayerCharacter's BGSActorCellEvent, kEnter): scan nearby NPCs soon.
        class CellSink : public RE::BSTEventSink<RE::BGSActorCellEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::BGSActorCellEvent* a_event, RE::BSTEventSource<RE::BGSActorCellEvent>*) override
            {
                if (a_event && a_event->flags == RE::BGSActorCellEvent::CellFlag::kEnter && g_inGame.load()) {
                    AutoScanSoon();
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // A follower's 3D loaded = the actor script's OnLoad: update it after the apply delay.
        class LoadSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* a_event, RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override
            {
                if (!a_event || !a_event->loaded || !g_inGame.load()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                int i = -1;
                {
                    std::lock_guard lock(g_lock);
                    for (int k = 0; k < static_cast<int>(g_slots.size()); ++k) {
                        if (g_slots[k].ref && g_slots[k].ref == a_event->formID) {
                            i = k;
                            break;
                        }
                    }
                }
                if (i >= 0) {
                    if (i < static_cast<int>(g_fullOnLoad.size()) && g_fullOnLoad[i]) {
                        g_fullOnLoad[i] = 0;
                        ScheduleUpdate(i, true);  // Apply to All while it was not loaded
                    } else {
                        ScheduleUpdate(i, false);
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // A follower's package started = the actor script's OnPackageStart: set its name again (the game
        // tends to drop it; the old script did the same).
        class PackageSink : public RE::BSTEventSink<RE::TESPackageEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::TESPackageEvent* a_event, RE::BSTEventSource<RE::TESPackageEvent>*) override
            {
                if (!a_event || a_event->type != 0 || !g_inGame.load()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                auto ref = a_event->actor.get();
                if (const int i = SlotOfActor(ref ? ref->As<RE::Actor>() : nullptr); i >= 0) {
                    OnMain([i]() { ApplyName(i); });  // ApplyName only writes when the name differs
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

        // Shared code of Save All / Save Each: the follower's data as the NPC files store it.
        Files::NPCData ToNPCData(int a_slot)
        {
            Files::NPCData n;
            auto           actor = ActorOf(a_slot);
            {
                std::lock_guard lock(g_lock);
                const auto&     s = g_slots[a_slot];
                n.actor = s.ref;
                n.slotId = a_slot;
                n.presetName = s.name;
                n.preset = s.preset;
                n.headMesh = s.headMesh;
                n.erinPreset = s.erinPreset;
                n.female = s.female ? 1 : 0;
                n.style = s.style;
                n.voice = s.voice;
                n.race = s.race;
                n.npcClass = s.npcClass;
                n.spells = s.spells;
            }
            if (actor) {
                for (auto g : WornGear(actor)) {
                    n.equipment.push_back(g->GetFormID());
                }
            }
            return n;
        }

        void SummonUntilArrived(int a_slot, int a_tries);

        // The ConfigManager's loadNPCActorVars + loadNPCSpellVars + loadNPCGear for one slot.
        // a_summon (NG, user: Pre Summon of a kept follower): summoned to the player after the full apply.
        void ApplyNPCData(int a_slot, const Files::NPCData& a_n, bool a_summon = false)
        {
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            RemoveSlotSpells(a_slot);
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                if (!a_n.presetName.empty()) {
                    s.name = a_n.presetName;
                }
                s.preset = PresetKey(a_n.preset);
                s.headMesh = a_n.headMesh;
                s.erinPreset = a_n.erinPreset;
                if (Lookup<RE::TESCombatStyle>(a_n.style)) {
                    s.style = a_n.style;
                }
                if (Lookup<RE::BGSVoiceType>(a_n.voice)) {
                    s.voice = a_n.voice;
                }
                if (Lookup<RE::TESRace>(a_n.race)) {
                    s.race = a_n.race;
                }
                if (Lookup<RE::TESClass>(a_n.npcClass)) {
                    s.npcClass = a_n.npcClass;
                }
                if (a_n.female >= 0) {
                    s.female = a_n.female != 0;  // NG: the slot takes the saved sex (old mod refused a mismatch)
                }
            }
            for (auto id : a_n.spells) {
                AddSlotSpell(a_slot, Lookup<RE::SpellItem>(id));
            }
            ApplyBase(a_slot);

            // enable, gear (UnequipAll, AddItem + EquipItemEx each, QueueNiNodeUpdate), then the full preset
            auto chain = Papyrus::Chain::Make();
            if (actor->IsDisabled()) {
                chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "Enable", d, false); });
            }
            chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "UnequipAll", d); });
            for (auto id : a_n.equipment) {
                auto item = Lookup(id);
                if (!item) {
                    continue;
                }
                chain->Then([actor, item](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "AddItem", d, item, std::int32_t(1), true); });
                chain->Then([actor, item](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "EquipItemEx", d, item, std::int32_t(0), false, false); });
            }
            chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "QueueNiNodeUpdate", d); });
            chain->ThenDo([a_slot, a_summon]() {
                OnMain([a_slot, a_summon]() {
                    if (a_summon) {
                        RefreshAppearance(a_slot, true, false, false, [a_slot]() { SummonUntilArrived(a_slot, 8); });
                    } else {
                        RefreshAppearance(a_slot, true);
                    }
                });
            });
            chain->Run();
        }

        // ---- Keep in New Games (NG, user: back, but only once RaceMenu is closed - "sometimes it bugs out") ----

        // The followers with Keep in New Game ticked -> LazyFollowersNGKeep.json (LazyFollowersNPC.json's layout).
        // A taken-over real NPC (e.g. Sofia) gets "RealNPC": true - a new game takes it over again (RestoreKept).
        // Nothing ticked and no file yet: no file written. Main thread.
        void WriteKeepFile()
        {
            nlohmann::json j = nlohmann::json::object();
            int            n = 0;
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                bool active = false, real = false, summon = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active() && g_slots[i].keep && g_slots[i].ref;
                    real = g_slots[i].real;
                    summon = g_slots[i].preSummon;
                }
                if (active) {
                    auto v = Files::NPCToJson(ToNPCData(i), true);
                    if (real) {
                        v["RealNPC"] = true;
                    }
                    v["PreSummon"] = summon;
                    j["NPC" + std::to_string(n++)] = std::move(v);
                }
            }
            std::error_code ec;
            if (n == 0 && !std::filesystem::exists(Files::KeepFile(), ec)) {
                return;
            }
            if (Files::WriteJson(Files::KeepFile(), j)) {
                Out(std::format("Keep in New Game: {} followers written", n));
            } else {
                logger::warn("Keep in New Game: could not write {}", Files::KeepFile().string());
            }
        }

        // The kept followers -> their slots (each: enable, gear, full preset), then the player's face refresh
        // that ends every pass. Main thread, new game only.
        void RestoreKept()
        {
            auto j = Files::ReadJson(Files::KeepFile());
            int  n = 0;
            if (j) {
                std::vector<int>                              used;
                std::vector<std::pair<Files::NPCData, bool>> reals;  // + Pre Summon
                for (auto& [key, value] : j->items()) {
                    auto       d = Files::NPCFromJson(value);
                    const auto ps = value.is_object() ? Files::Find(value, "PreSummon") : nullptr;
                    const bool summon = ps && ps->is_boolean() && ps->get<bool>();
                    if (d && value.is_object() && value.contains("RealNPC")) {
                        reals.emplace_back(std::move(*d), summon);
                        continue;
                    }
                    if (!d || d->slotId < 0 || d->slotId >= g_lfCount || std::find(used.begin(), used.end(), d->slotId) != used.end()) {
                        continue;
                    }
                    used.push_back(d->slotId);
                    Out(std::format("Keep in New Game: {} -> follower {}", d->presetName, d->slotId + 1));
                    ApplyNPCData(d->slotId, *d, summon);
                    {
                        std::lock_guard lock(g_lock);
                        g_slots[d->slotId].keep = true;  // stays ticked in the new game
                        g_slots[d->slotId].preSummon = summon;
                    }
                    ++n;
                }
                // NG (user: "it should work, that's what we have been doing"): a kept take-over (e.g. Sofia with
                // Ciel.jslot) is done again exactly like Apply Settings with Override ticked - a free slot gets name,
                // preset, race and the NPC's voice, Apply finds the voice's NPC and it takes over (preset, brought to
                // the player). Its kept worn gear is put on it a few seconds later (after the take-over's gear swap).
                for (auto& [d, summon] : reals) {
                    int freeSlot = -1;
                    {
                        std::lock_guard lock(g_lock);
                        for (int i = 0; i < g_lfCount; ++i) {
                            if (!g_slots[i].Listed() && std::find(used.begin(), used.end(), i) == used.end()) {
                                freeSlot = i;
                                break;
                            }
                        }
                    }
                    const auto preset = PresetKey(d.preset);
                    if (freeSlot < 0 || preset.empty() || !Lookup<RE::BGSVoiceType>(d.voice)) {
                        logger::warn("Keep in New Game: {} not taken over again (no free slot, preset or voice)", d.presetName);
                        continue;
                    }
                    used.push_back(freeSlot);
                    Out(std::format("Keep in New Game: {} - taken over again with {}.jslot (voice {:08X})", d.presetName, preset, d.voice));
                    ApplyRequest req{ d.presetName, d.race, d.voice, preset + ".jslot", true, true, summon };
                    req.erinPreset = d.erinPreset;
                    Apply(freeSlot, std::move(req));
                    if (auto owner = Lookup<RE::Actor>(d.actor); owner && !d.equipment.empty()) {
                        After(6.0f, [owner, gear = d.equipment]() {
                            OnMain([owner, gear]() {
                                auto chain = Papyrus::Chain::Make();
                                for (auto id : gear) {
                                    auto item = Lookup(id);
                                    if (!item) {
                                        continue;
                                    }
                                    chain->Then([owner, item](Papyrus::Done a_d) {
                                        return Papyrus::CallMethod(owner, "ObjectReference", "AddItem", a_d, item, std::int32_t(1), true);
                                    });
                                    chain->Then([owner, item](Papyrus::Done a_d) {
                                        return Papyrus::CallMethod(owner, "Actor", "EquipItemEx", a_d, item, std::int32_t(0), false, false);
                                    });
                                }
                                chain->Run();
                            });
                        });
                    }
                    ++n;
                }
            }
            BuildLists();
            Out(std::format("Keep in New Game: {} followers brought back", n));
            if (n > 0) {
                SetNotice(std::format("Keep in New Game: {} followers brought back (Summon All brings them to you).", n));
            }
        }

        // RaceMenu was closed in a new game: once nothing is loading and RaceMenu has not opened again, the kept
        // followers come back (0.5 s after the close, so RaceMenu's own work on the player is done).
        void TryRestoreKept(int a_tries)
        {
            OnMain([a_tries]() {
                if (!g_keepPending.load()) {
                    return;
                }
                auto ui = RE::UI::GetSingleton();
                if (ui && ui->IsMenuOpen(RE::RaceSexMenu::MENU_NAME)) {
                    return;  // opened again: its next close tries again
                }
                if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) && a_tries > 0) {
                    After(1.0f, [a_tries]() { TryRestoreKept(a_tries - 1); });
                    return;
                }
                g_keepPending = false;
                RestoreKept();
            });
        }

        // RaceMenu (the game's RaceSex Menu) closed while a new game waits for its kept followers.
        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && !a_event->opening && g_keepPending.load() && a_event->menuName == RE::RaceSexMenu::MENU_NAME) {
                    Out("Keep in New Game: RaceMenu closed - followers come back in 0.5 s");
                    After(0.5f, []() { TryRestoreKept(30); });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };

    }

    // ==================================================================
    // setup
    // ==================================================================

    void LoadForms()
    {
        auto dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !dh->LookupModByName(kPlugin)) {
            logger::error("{} is not loaded - Lazy Followers NG does nothing", kPlugin);
            return;
        }
        auto npcs = dh->LookupForm<RE::BGSListForm>(kNPCsList, kPlugin);
        auto styles = dh->LookupForm<RE::BGSListForm>(kStylesList, kPlugin);
        g_femaleVoicesList = dh->LookupForm<RE::BGSListForm>(kFemaleVoicesList, kPlugin);
        g_maleVoicesList = dh->LookupForm<RE::BGSListForm>(kMaleVoicesList, kPlugin);
        g_spellsList = dh->LookupForm<RE::BGSListForm>(kSpellsList, kPlugin);
        g_classesList = dh->LookupForm<RE::BGSListForm>(kClassesList, kPlugin);
        g_faction = dh->LookupForm<RE::TESFaction>(kFaction, kPlugin);
        g_marker = dh->LookupForm<RE::TESObjectREFR>(kCellMarker, kPlugin);
        g_followerFaction = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);
        g_hairColor = dh->LookupForm<RE::BGSColorForm>(0x801, "RaceMenu.esp");
        if (dh->LookupModByName("FGT.esp")) {
            g_fgtQuest = dh->LookupForm<RE::TESQuest>(0x12CA, "FGT.esp");
        }
        for (int i = 0; i < static_cast<int>(Global::kCount); ++i) {
            g_globals[i] = dh->LookupForm<RE::TESGlobal>(kGlobalIDs[i], kPlugin);
        }
        if (!npcs || !styles || std::any_of(g_globals.begin(), g_globals.end(), [](auto g) { return !g; })) {
            logger::error("{}: forms missing (not version 0.42.3?) - Lazy Followers NG does nothing", kPlugin);
            return;
        }

        auto styleForms = ListForms(styles);
        for (int i = 0; i < kStyles && i < static_cast<int>(styleForms.size()); ++i) {
            g_styles[i] = styleForms[i] ? styleForms[i]->As<RE::TESCombatStyle>() : nullptr;
            g_styleDefaults[i] = ReadStyle(g_styles[i]);
            g_styleNames[i] = kStyleDefaultNames[i];
        }

        std::lock_guard lock(g_lock);
        for (auto f : ListForms(npcs)) {
            auto actor = f ? f->As<RE::Actor>() : nullptr;
            auto npc = actor ? actor->GetActorBase() : nullptr;
            if (!npc) {
                continue;
            }
            Slot s;
            s.ref = actor->GetFormID();
            s.base = npc->GetFormID();
            s.defaultName = npc->GetFullName();
            s.defaultRace = IdOf(npc->race);
            s.defaultVoice = IdOf(npc->voiceType);
            s.defaultStyle = IdOf(npc->combatStyle);
            s.defaultClass = IdOf(npc->npcClass);
            s.defaultFemale = npc->IsFemale();
            s.defaultHair = IdOf(npc->headRelatedData ? npc->headRelatedData->hairColor : nullptr);
            s.female = s.defaultFemale;
            s.name = s.defaultName;
            s.race = s.defaultRace;
            s.voice = s.defaultVoice;
            s.style = s.defaultStyle;
            s.npcClass = s.defaultClass;
            g_slots.push_back(std::move(s));
            g_actors.push_back(actor);
            g_fullOnLoad.push_back(0);
        }
        g_lfCount = static_cast<int>(g_slots.size());
        g_formsFound = g_lfCount > 0;
        if (g_formsFound) {
            KeepHeadData();  // NG 1.2: real NPC followers' own head data (Remove)
        }
        for (int i = 0; g_formsFound && i < kRealSlots; ++i) {
            Slot r;
            r.real = true;  // free real NPC entry (ClaimReal)
            g_slots.push_back(std::move(r));
            g_actors.push_back(nullptr);
            g_fullOnLoad.push_back(0);
        }
        logger::info("{}: {} follower slots, RaceMenu hair colour {}, Follower Goes on a Trip {}", kPlugin, g_lfCount, g_hairColor ? "found" : "missing",
            g_fgtQuest ? "found" : "not installed");
    }

    bool FormsFound() { return g_formsFound; }

    void InstallSinks()
    {
        static CrosshairSink cross;
        if (auto src = SKSE::GetCrosshairRefEventSource()) {
            src->AddEventSink(&cross);
        }
        static LoadSink load;
        if (auto holder = RE::ScriptEventSourceHolder::GetSingleton()) {
            holder->AddEventSink<RE::TESObjectLoadedEvent>(&load);
            static PackageSink package;
            holder->AddEventSink<RE::TESPackageEvent>(&package);
        }
        static CellSink cell;
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            player->AsBGSActorCellEventSource()->AddEventSink(&cell);
        }
        static MenuSink menu;  // Keep in New Games: RaceMenu closed
        if (auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&menu);
        }
    }

    void RegisterCosave()
    {
        auto s = SKSE::GetSerializationInterface();
        s->SetUniqueID(kCosaveID);
        s->SetSaveCallback(OnSave);
        s->SetLoadCallback(OnLoad);
        s->SetRevertCallback(OnRevert);
    }

    // kPostPostLoad (the Erin race registered its listener at kPostLoad): asks MuErinRace.dll for its interface.
    void InitErin()
    {
        void* iface = nullptr;
        auto  messaging = SKSE::GetMessagingInterface();
        if (!messaging || !messaging->Dispatch(kErinRequest, &iface, sizeof(iface), "MuErinRace") || !iface) {
            logger::info("Erin race (MuErinRace.dll) not found - no Erin Preset row");
            return;
        }
        auto       vtbl = *reinterpret_cast<void***>(iface);
        const auto version = reinterpret_cast<std::uint32_t (*)(void*)>(vtbl[1])(iface);
        if (version < kErinMinVersion) {
            logger::warn("Erin race interface version {} is older than {} - no Erin Preset row", version, kErinMinVersion);
            return;
        }
        g_erin = iface;
        logger::info("Erin race interface found (version {}) - Erin Preset available", version);
    }

    // The player script's OnPlayerLoadGame (+ the MCM's setCSValues): base edits on every follower now,
    // presets on the loaded ones after the delay.
    void OnGameLoaded()
    {
        if (!g_formsFound) {
            return;
        }
        ++g_generation;
        g_inGame = true;
        g_dialogueVoices.clear();  // form-list additions of the last game are gone: add again
        LoadSettings();  // global settings over the save's (before ApplyBase: combat styles / classes on or off)
        ApplyLogLevel();
        for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
            ApplyBase(i);
        }
        BuildLists();
        for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
            bool active = false;
            {
                std::lock_guard lock(g_lock);
                active = g_slots[i].Active();
            }
            if (auto a = ActorOf(i); active && a && a->Is3DLoaded()) {
                ScheduleUpdate(i, false);
            }
        }
        AutoScanSoon();  // NG (user): Nearby Actors list for this save
    }

    void OnLeaveGame()
    {
        g_inGame = false;
        g_keepPending = false;
        ++g_generation;
    }

    // NG (user): a new game is set up like a load; followers in the keep file (Keep in New Game ticked at the last
    // save) come back once RaceMenu has been closed (MenuSink).
    void OnNewGame()
    {
        OnGameLoaded();
        g_keepPending = false;
        auto       j = Files::ReadJson(Files::KeepFile());
        const auto n = j && j->is_object() ? j->size() : 0;
        if (n > 0) {
            Out(std::format("Keep in New Game: new game, {} followers kept - they come back after RaceMenu is closed", n));
            g_keepPending = true;
        }
    }

    // NG (user): every save writes the followers with Keep in New Game ticked (not while a new game still waits for
    // them: its early saves would empty the file).
    void OnGameSaved()
    {
        if (!g_keepPending.load() && g_inGame.load()) {
            WriteKeepFile();
        }
    }

    // The follower's Pre Summon box (under Keep in New Game); the file is written at once.
    void SetPreSummon(int a_slot, bool a_on)
    {
        OnMain([a_slot, a_on]() {
            if (a_slot < 0 || a_slot >= static_cast<int>(g_slots.size())) {
                return;
            }
            {
                std::lock_guard lock(g_lock);
                g_slots[a_slot].preSummon = a_on;
            }
            if (g_inGame.load() && !g_keepPending.load()) {
                WriteKeepFile();
            }
        });
    }

    // The follower's Keep in New Game box; the file is written at once (no wait for the next save).
    void SetKeep(int a_slot, bool a_on)
    {
        OnMain([a_slot, a_on]() {
            if (a_slot < 0 || a_slot >= static_cast<int>(g_slots.size())) {
                return;
            }
            {
                std::lock_guard lock(g_lock);
                g_slots[a_slot].keep = a_on;
            }
            if (g_inGame.load() && !g_keepPending.load()) {
                WriteKeepFile();
            }
        });
    }

    bool InGame() { return g_inGame.load(); }

    // ==================================================================
    // menu
    // ==================================================================

    // NG perf: a new snapshot at most every 100 ms (was every frame), at once when another follower is picked.
    void RequestSnapshot(int a_selected)
    {
        static std::atomic<std::int64_t> s_last{ 0 };
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool changed = g_snapSelected.exchange(a_selected) != a_selected;
        if (!g_formsFound || (!changed && now - s_last.load() < 100) || g_snapQueued.exchange(true)) {
            return;
        }
        s_last = now;
        OnMain([]() {
            BuildSnapshot();
            g_snapQueued = false;
        });
    }

    std::shared_ptr<const Snapshot> GetSnapshot()
    {
        std::lock_guard lock(g_snapLock);
        return g_snap;
    }

    int SlotCount()
    {
        std::lock_guard lock(g_lock);
        return static_cast<int>(g_slots.size());
    }

    void OnFollowersPageOpened()
    {
        OnMain([]() {
            if (!g_inGame) {
                return;
            }
            Meet(RE::PlayerCharacter::GetSingleton());
            if (auto a = CrosshairActor()) {
                const int slot = SlotOfActor(a);
                if (slot >= 0) {
                    g_selectRequest = slot;  // the MCM selected the follower under the crosshair
                } else {
                    Meet(a);
                }
            }
            BuildLists();
        });
    }

    // The slot the pages should select (-1 = none); a_moved: Apply moved the follower to it (take-over / give-back).
    int TakeSelect(bool* a_moved)
    {
        const int  sel = g_selectRequest.exchange(-1);
        const bool moved = g_selectMoved.exchange(false);
        if (a_moved) {
            *a_moved = sel >= 0 && moved;
        }
        return sel;
    }

    unsigned GameLoads() { return g_generation.load(); }

    // The first slot not in the Followers list joins it (no preset yet) and gets selected.
    void AddFollower()
    {
        OnMain([]() {
            int slot = -1;
            {
                std::lock_guard lock(g_lock);
                for (int i = 0; i < g_lfCount; ++i) {
                    if (!g_slots[i].Listed()) {
                        g_slots[i].added = true;
                        slot = i;
                        break;
                    }
                }
            }
            if (slot < 0) {
                SetNotice(std::format("All {} follower slots are in use.", g_lfCount), true);
                return;
            }
            g_selectRequest = slot;
            SetNotice(std::format("Follower {} added - pick a preset and Apply Settings.", slot + 1));
        });
    }

    bool Nickname(RE::FormID a_ref, std::string& a_from, std::string& a_to)
    {
        if (!a_ref) {
            return false;
        }
        std::lock_guard lock(g_lock);
        for (int i = g_lfCount; i < static_cast<int>(g_slots.size()); ++i) {
            const auto& s = g_slots[i];
            if (s.ref != a_ref) {
                continue;
            }
            if (!s.Active() || s.name.empty() || s.defaultName.empty() || s.name == s.defaultName) {
                return false;
            }
            a_from = s.defaultName;
            a_to = s.name;
            return true;
        }
        return false;
    }

    void RebuildLists()
    {
        OnMain([]() { BuildLists(); });
    }

    std::string Notice(bool& a_error)
    {
        std::lock_guard lock(g_noticeLock);
        a_error = g_noticeError;
        return g_notice;
    }

    void ClearNotice() { SetNotice({}); }

    void SetNotice(std::string a_text, bool a_error)
    {
        if (!a_text.empty()) {
            if (a_error) {
                logger::warn("{}", a_text);
            } else {
                logger::info("{}", a_text);
            }
        }
        std::lock_guard lock(g_noticeLock);
        g_notice = std::move(a_text);
        g_noticeError = a_error;
    }

    // ==================================================================
    // settings
    // ==================================================================

    int GetGlobal(Global a_g)
    {
        auto g = g_globals[static_cast<int>(a_g)];
        return g ? static_cast<int>(g->value) : GlobalDefault(a_g);
    }

    void SetGlobal(Global a_g, int a_value, bool a_save)
    {
        if (auto g = g_globals[static_cast<int>(a_g)]; g && g->value != static_cast<float>(a_value)) {
            g->value = static_cast<float>(a_value);
            if (a_g == Global::kTrace) {
                ApplyLogLevel();
            }
            if (a_save) {
                SaveSettings();
            }
        }
    }

    void SaveSettingsFile() { SaveSettings(); }

    int GlobalDefault(Global a_g) { return kGlobalDefaults[static_cast<int>(a_g)]; }

    bool StylesEnabled()
    {
        std::lock_guard lock(g_lock);
        return g_stylesEnabled;
    }

    void SetStylesEnabled(bool a_on)
    {
        {
            std::lock_guard lock(g_lock);
            g_stylesEnabled = a_on;
        }
        SaveSettings();
        Out(a_on ? "Custom combat styles ENABLED" : "Custom combat styles DISABLED");
    }

    bool ClassesEnabled()
    {
        std::lock_guard lock(g_lock);
        return g_classesEnabled;
    }

    void SetClassesEnabled(bool a_on)
    {
        {
            std::lock_guard lock(g_lock);
            g_classesEnabled = a_on;
        }
        SaveSettings();
        Out(a_on ? "Custom classes ENABLED" : "Custom classes DISABLED");
    }

    bool SpellsEnabled()
    {
        std::lock_guard lock(g_lock);
        return g_spellsEnabled;
    }

    void SetSpellsEnabled(bool a_on)
    {
        {
            std::lock_guard lock(g_lock);
            g_spellsEnabled = a_on;
        }
        SaveSettings();
    }

    float ScanRadius()
    {
        std::lock_guard lock(g_lock);
        return g_scanRadius;
    }

    void SetScanRadius(float a_meters)
    {
        {
            std::lock_guard lock(g_lock);
            g_scanRadius = std::clamp(a_meters, 1.0f, 100.0f);
        }
        SaveSettings();
    }

    // Every Settings page value back to its default (one file write).
    void ResetSettings()
    {
        for (int i = 0; i < static_cast<int>(Global::kCount); ++i) {
            if (auto g = g_globals[i]) {
                g->value = static_cast<float>(kGlobalDefaults[i]);
            }
        }
        {
            std::lock_guard lock(g_lock);
            g_stylesEnabled = true;
            g_classesEnabled = true;
            g_spellsEnabled = true;
            g_autoEquip = false;
            g_includeEquip = true;
            g_scanRadius = kDefaultScanRadius;
            g_nearbyRows = kDefaultNearbyRows;
            g_presetRows = kDefaultNearbyRows;
        }
        ApplyLogLevel();
        SaveSettings();
        OnMain([]() {
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                ApplyBase(i);  // combat styles / classes back on
            }
        });
        SetNotice("All settings are back to their defaults.");
    }

    void LoadEarlySettings()
    {
        if (auto j = Files::ReadJson(Files::SettingsFile())) {
            LoadUiSettings(*j);
        }
    }

    KeyBinding OpenWindowKey()
    {
        const auto v = g_openKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetOpenWindowKey(KeyBinding a_key)
    {
        g_openKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    KeyBinding SummonKey()
    {
        const auto v = g_summonKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetSummonKey(KeyBinding a_key)
    {
        g_summonKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    KeyBinding SummonAllKey()
    {
        const auto v = g_summonAllKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetSummonAllKey(KeyBinding a_key)
    {
        g_summonAllKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    KeyBinding SavePresetKey()
    {
        const auto v = g_savePresetKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetSavePresetKey(KeyBinding a_key)
    {
        g_savePresetKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    KeyBinding OpenInventoryKey()
    {
        const auto v = g_openInvKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetOpenInventoryKey(KeyBinding a_key)
    {
        g_openInvKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    KeyBinding NearbyWindowKey()
    {
        const auto v = g_nearbyWinKey.load();
        return { static_cast<std::uint32_t>(v & 0xFFFFFFFF), static_cast<std::uint32_t>(v >> 32) };
    }

    void SetNearbyWindowKey(KeyBinding a_key)
    {
        g_nearbyWinKey = static_cast<std::uint64_t>(a_key.vk) | (static_cast<std::uint64_t>(a_key.mods) << 32);
        SaveSettings();
    }

    std::vector<float> NearbyWidths()
    {
        std::lock_guard lock(g_lock);
        return g_nearbyWidths;
    }

    void SetNearbyWidths(const std::vector<float>& a_widths)
    {
        {
            std::lock_guard lock(g_lock);
            g_nearbyWidths = a_widths;
        }
        SaveSettings();
    }

    std::vector<float> PresetWidths()
    {
        std::lock_guard lock(g_lock);
        return g_presetWidths;
    }

    void SetPresetWidths(const std::vector<float>& a_widths)
    {
        {
            std::lock_guard lock(g_lock);
            g_presetWidths = a_widths;
        }
        SaveSettings();
    }

    int PresetRows()
    {
        std::lock_guard lock(g_lock);
        return g_presetRows;
    }

    void SetPresetRows(int a_rows)
    {
        {
            std::lock_guard lock(g_lock);
            g_presetRows = std::clamp(a_rows, 1, 50);
        }
        SaveSettings();
    }

    int NearbyRows()
    {
        std::lock_guard lock(g_lock);
        return g_nearbyRows;
    }

    void SetNearbyRows(int a_rows)
    {
        {
            std::lock_guard lock(g_lock);
            g_nearbyRows = std::clamp(a_rows, 1, 50);
        }
        SaveSettings();
    }

    bool AutoEquip()
    {
        std::lock_guard lock(g_lock);
        return g_autoEquip;
    }

    void SetAutoEquip(bool a_on)
    {
        {
            std::lock_guard lock(g_lock);
            g_autoEquip = a_on;
        }
        SaveSettings();
    }

    bool IncludeEquipment()
    {
        std::lock_guard lock(g_lock);
        return g_includeEquip;
    }

    void SetIncludeEquipment(bool a_on)
    {
        {
            std::lock_guard lock(g_lock);
            g_includeEquip = a_on;
        }
        SaveSettings();
    }

    bool AllSpellsLoaded()
    {
        std::lock_guard lock(g_lock);
        return g_allSpells;
    }

    // ==================================================================
    // follower actions
    // ==================================================================

    namespace
    {
        // a_actor is in the player's cell / worldspace and within ~14 m.
        bool NextToPlayer(RE::Actor* a_actor)
        {
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!a_actor || !player) {
                return false;
            }
            const bool sameSpace = a_actor->GetParentCell() == player->GetParentCell() ||
                                   (a_actor->GetWorldspace() && a_actor->GetWorldspace() == player->GetWorldspace());
            return sameSpace && a_actor->GetPosition().GetDistance(player->GetPosition()) < 1000.0f;
        }

        // Fix (user log, 2nd try): on a follower fresh from storage, Apply Settings' MoveTo did nothing - right
        // after it returned the follower was still in storage (11104 units away), and waiting did not help -
        // while Summon NPC pressed AFTER the preset was applied worked at once. So Apply Settings now does the
        // full apply first and summons afterwards (what worked by hand); a summon that leaves the follower
        // away is tried again (a_tries times, 0.25 s apart). Then the usual lite pass.
        void SummonUntilArrived(int a_slot, int a_tries);

        // NG 1.2 (user: "Ciel's gear, drop Sofia's - for all modded voices"): taking over, the real NPC drops its own
        // armor / weapons / ammo (quest items stay), then gets everything the slot follower carried and wears what
        // it wore. Its default outfit is off meanwhile (ApplyRealSkin), so the game does not dress it again;
        // Remove puts the outfit back on.
        void TransferGear(RE::Actor* a_from, RE::Actor* a_to)
        {
            if (!a_from || !a_to) {
                return;
            }
            auto                                                  worn = WornGear(a_from);
            std::vector<std::pair<RE::TESBoundObject*, std::int32_t>> drop;
            for (auto& [obj, data] : a_to->GetInventory()) {
                if (obj && data.first > 0 && (obj->IsArmor() || obj->IsWeapon() || obj->IsAmmo()) && !(data.second && data.second->IsQuestObject())) {
                    drop.emplace_back(obj, data.first);
                }
            }
            auto chain = Papyrus::Chain::Make();
            chain->Then([a_to](Papyrus::Done d) { return Papyrus::CallMethod(a_to, "Actor", "UnequipAll", d); });
            chain->Then([a_to, drop](Papyrus::Done d) {
                OnMain([a_to, drop, d]() {
                    for (auto& [obj, count] : drop) {
                        a_to->RemoveItem(obj, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
                    }
                    Out(std::format("{} - {} own armor / weapon items dropped", DisplayName(a_to), drop.size()));
                    d(Papyrus::Variable{});
                });
                return true;
            });
            chain->Then([a_from, a_to](Papyrus::Done d) {
                return Papyrus::CallMethod(a_from, "ObjectReference", "RemoveAllItems", d, static_cast<RE::TESObjectREFR*>(a_to), false, false);
            });
            for (auto item : worn) {
                chain->Then([a_to, item](Papyrus::Done d) { return Papyrus::CallMethod(a_to, "Actor", "EquipItem", d, static_cast<RE::TESForm*>(item), false, true); });
            }
            chain->ThenDo([from = DisplayName(a_from), to = DisplayName(a_to), n = worn.size()]() {
                Out(std::format("{} got {}'s items ({} worn)", to, from, n));
            });
            chain->Run();
        }

        // NG 1.2: a real NPC (e.g. Sofia) as a real NPC entry: its entry if it has one, else a free entry filled
        // with its own values. -1 = a Lazy Followers slot actor or no free entry. Main thread.
        // A real NPC's reference name data as it is now (before we rename it) -> a_slot's text fields.
        void CaptureText(RE::Actor* a_actor, Slot& a_slot)
        {
            a_slot.textSaved = true;
            auto x = a_actor->extraList.GetByType<RE::ExtraTextDisplayData>();
            a_slot.textHad = x != nullptr;
            if (x) {
                a_slot.textName = x->displayName.c_str();
                a_slot.textMessage = IdOf(x->displayNameText);
                a_slot.textQuest = IdOf(x->ownerQuest);
                a_slot.textInstance = static_cast<std::int32_t>(x->ownerInstance.underlying());
                a_slot.textTemper = x->temperFactor;
            }
        }

        // The captured reference name data back (give-back). Main thread.
        void RestoreText(RE::Actor* a_actor, const Slot& a_slot)
        {
            auto& list = a_actor->extraList;
            if (!a_slot.textHad) {
                list.RemoveByType(RE::ExtraDataType::kTextDisplayData);
                return;
            }
            auto x = list.GetByType<RE::ExtraTextDisplayData>();
            if (!x) {
                a_actor->SetDisplayName(a_slot.textName, true);  // creates it
                x = list.GetByType<RE::ExtraTextDisplayData>();
            }
            if (x) {
                x->displayName = a_slot.textName;
                x->customNameLength = static_cast<std::uint16_t>(a_slot.textName.size());
                x->displayNameText = Lookup<RE::BGSMessage>(a_slot.textMessage);
                x->ownerQuest = Lookup<RE::TESQuest>(a_slot.textQuest);
                x->ownerInstance = static_cast<RE::ExtraTextDisplayData::DisplayDataType>(a_slot.textInstance);
                x->temperFactor = a_slot.textTemper;
            }
        }

        int ClaimReal(RE::Actor* a_actor)
        {
            auto npc = a_actor ? a_actor->GetActorBase() : nullptr;
            if (!npc) {
                return -1;
            }
            if (const int slot = SlotOfActor(a_actor); slot >= 0) {
                return IsReal(slot) ? slot : -1;
            }
            std::lock_guard lock(g_lock);
            for (int i = g_lfCount; i < static_cast<int>(g_slots.size()); ++i) {
                if (g_slots[i].real && !g_slots[i].ref) {
                    Slot s;
                    s.real = true;
                    s.ref = a_actor->GetFormID();
                    s.base = npc->GetFormID();
                    s.defaultName = npc->GetName();
                    s.defaultRace = IdOf(a_actor->GetRace());
                    s.defaultFemale = npc->IsFemale();
                    OwnDefaults(npc, s);  // skin, outfit, voice, combat style, class
                    s.female = s.defaultFemale;
                    s.name = s.defaultName.empty() ? DisplayName(a_actor) : s.defaultName;
                    s.race = s.defaultRace;
                    s.voice = s.defaultVoice;
                    s.style = s.defaultStyle;
                    s.npcClass = s.defaultClass;
                    s.added = true;
                    CaptureText(a_actor, s);  // before it gets the follower's name
                    g_slots[i] = std::move(s);
                    return i;
                }
            }
            return -1;
        }

        // NG 1.2 (user: "the follower will get the real NPC's voice, name; they keep their face, race and the
        // other stuff of the .jslot"): the NPC of a modded voice's own mod that is in the game (e.g. Sofia for
        // JJSofiaVoiceType). Matched by its voice in the game or in its plugin file (Voice::PluginNPCs - Severa's
        // differs); unique and alive ones first. References made by scripts (FF......) and our slots are skipped.
        // nullptr = none (a voice pack without its own NPC: the voice is used on the slot as before).
        RE::Actor* FindVoiceOwner(RE::BGSVoiceType* a_voice)
        {
            if (!a_voice || !Voice::IsModded(a_voice)) {
                return nullptr;
            }
            const auto                           npcs = Voice::PluginNPCs(a_voice);
            const std::unordered_set<RE::FormID> fileNpcs(npcs.begin(), npcs.end());
            RE::Actor*                           best = nullptr;
            int                                  bestScore = -1;
            int                                  found = 0;
            const auto& [map, lock] = RE::TESForm::GetAllForms();
            const RE::BSReadLockGuard guard{ lock };
            for (auto& [id, form] : *map) {
                if (!form || form->GetFormType() != RE::FormType::ActorCharacter || (id >> 24) == 0xFF) {
                    continue;
                }
                auto actor = static_cast<RE::Actor*>(form);
                auto npc = actor->GetActorBase();
                auto placed = actor->GetBaseObject();
                auto file = npc ? npc->GetFile(0) : nullptr;
                if (!npc || actor->IsPlayerRef() || (file && file->GetFilename() == kPlugin)) {
                    continue;
                }
                if (npc->voiceType != a_voice && !fileNpcs.contains(npc->GetFormID()) && !(placed && fileNpcs.contains(placed->GetFormID()))) {
                    continue;
                }
                ++found;
                const int score = (npc->IsUnique() ? 2 : 0) + (actor->IsDead() ? 0 : 1);
                if (score > bestScore) {
                    best = actor;
                    bestScore = score;
                }
            }
            if (best) {
                Out(std::format("Voice {:08X}: its NPC in the game is {} ({:08X}, {} found)", a_voice->GetFormID(), DisplayName(best), best->GetFormID(), found));
            }
            return best;
        }

        // The actor script's dismissMe for a Lazy Followers slot: reset, move to the storage cell marker, disable.
        void StoreSlot(int a_slot)
        {
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            // NG fix: after the reset's RaceMenu clean-up and race change, in the same chain (they ran side by side)
            ResetSlot(a_slot, [actor](Papyrus::Chain& a_chain) {
                if (g_marker) {
                    a_chain.Then([actor](Papyrus::Done d) {
                        return Papyrus::CallMethod(actor, "ObjectReference", "MoveTo", d, g_marker, 0.0f, 0.0f, 0.0f, true);
                    });
                }
                a_chain.Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "Disable", d, false); });
            });
        }

        void SummonUntilArrived(int a_slot, int a_tries)
        {
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            SummonTo(actor, RE::PlayerCharacter::GetSingleton(), [a_slot, a_tries]() {
                auto a = ActorOf(a_slot);
                if (NextToPlayer(a)) {
                    ScheduleUpdate(a_slot, false, true);
                } else if (a_tries > 0) {
                    Out(std::format("{} is not next to you yet - summoning again", DisplayName(a)));
                    After(0.25f, [a_slot, a_tries]() { SummonUntilArrived(a_slot, a_tries - 1); });
                } else {
                    Out(std::format("{} could not be summoned - try Summon NPC", DisplayName(a)));
                }
            });
        }
    }

    // NG 1.2 (user): a Lazy Followers slot given a modded voice whose own NPC is in the game (e.g. Sofia's
    // voice): that NPC becomes the follower instead - it gets the preset (face, body...), race and sex, and keeps
    // its own voice, name, dialogue, scripts and follower quest (a real NPC entry, selected in the pages); the
    // slot is reset and goes back to storage. Only with "Override the modded follower voiced .esp" ticked (req.takeOver,
    // off by default); otherwise and for other voices: applied to the slot as before. NG 1.2 (user: after taking
    // over, the box was gone - no way back): unticked on a real NPC with its own modded voice (e.g. Sofia) gives
    // it back and a free slot becomes the follower with that voice (see below).
    void Apply(int a_from, ApplyRequest a_req)
    {
        OnMain([a_from, req = std::move(a_req)]() {
            int        a_slot = a_from;
            bool       takeOver = false;
            bool       gaveBack = false;
            bool       switched = false;  // NG: a taken-over NPC replaced by another voice's NPC
            std::string slotName;
            if (!ActorOf(a_slot) || req.presetFile.size() <= 6) {
                SetNotice("Pick a preset first.", true);
                return;
            }
            // NG 1.2 (user, option 3): only with the Override box ticked; otherwise the voice stays on the slot (its
            // lines swapped in) and the real NPC is left alone
            if (!IsReal(a_slot) && req.takeOver) {
                auto owner = FindVoiceOwner(Lookup<RE::BGSVoiceType>(req.voice));
                // NG 1.2 (user: "you can't use another follower with the NPC voice used by another"): the NPC already
                // carries another follower's preset -> this follower keeps the voice on its own slot (line swap)
                if (const int busy = SlotOfActor(owner); owner && busy >= 0) {
                    bool taken = false;
                    {
                        std::lock_guard lock(g_lock);
                        taken = g_slots[busy].real && g_slots[busy].Active();
                    }
                    if (taken) {
                        Out(std::format("{} already carries a follower's preset - voice {:08X} used on {} itself", DefaultName(busy), req.voice,
                            DisplayName(ActorOf(a_slot))));
                        owner = nullptr;
                    }
                }
                // NG (user: move the Override from another follower): the NPC is still being given back -> try again
                // in 0.5 s (20 s at most), so its give-back (own look, sent home) can't land after the new take-over
                if (owner && g_restoring.contains(owner->GetFormID())) {
                    if (req.tries >= 40) {
                        SetNotice(std::format("{} is still being given back - try Apply Settings again.", DisplayName(owner)), true);
                        return;
                    }
                    if (req.tries == 0) {  // NG (user: re-applying) - say it waits, so Apply is not pressed again
                        SetNotice(std::format("Waiting for {} to be given back...", DisplayName(owner)));
                    }
                    auto next = req;
                    ++next.tries;
                    After(0.5f, [a_from, next]() { Apply(a_from, next); });
                    return;
                }
                if (owner) {
                    if (auto mu = MuConflict(owner); !mu.empty()) {
                        Out(mu);
                        SetNotice(mu, true);
                        return;
                    }
                    const int r = ClaimReal(owner);
                    if (r < 0) {
                        SetNotice(std::format("{} could not take over (all {} real NPC entries are in use).", DisplayName(owner), kRealSlots), true);
                        return;
                    }
                    slotName = DisplayName(ActorOf(a_slot));
                    Out(std::format("{} takes over {} (voice {:08X}); the slot goes back to storage", DisplayName(owner), slotName, req.voice));
                    TransferGear(ActorOf(a_slot), owner);  // before StoreSlot's reset (worn list)
                    StoreSlot(a_slot);
                    a_slot = r;
                    takeOver = true;
                    g_selectMoved = true;
                    g_selectRequest = r;
                }
            }
            // NG fix (user video 15-09-22: on Vicky = Cubbi taken over, Sofia's voice picked + Override ticked + Apply only
            // gave Cubbi Sofia's voice type - Cubbi went quiet, Sofia stayed as she was): a taken-over NPC whose voice is
            // switched to ANOTHER modded follower's voice switches NPC - the old one is given back (own look, voice,
            // name, sent home), the voice's own NPC takes over with this follower's settings, gear and Keep boxes.
            if (!takeOver && IsReal(a_slot) && req.takeOver) {
                auto old = ActorOf(a_slot);
                auto owner = FindVoiceOwner(Lookup<RE::BGSVoiceType>(req.voice));
                if (owner && old && owner != old) {
                    if (const int busy = SlotOfActor(owner); busy >= 0) {
                        SetNotice(std::format("{} is already a follower - pick another voice or Remove that follower first.", DisplayName(owner)), true);
                        return;
                    }
                    if (g_restoring.contains(owner->GetFormID())) {  // still being given back: try again in 0.5 s (20 s at most)
                        if (req.tries >= 40) {
                            SetNotice(std::format("{} is still being given back - try Apply Settings again.", DisplayName(owner)), true);
                            return;
                        }
                        if (req.tries == 0) {  // NG (user: re-applying) - say it waits, so Apply is not pressed again
                            SetNotice(std::format("Waiting for {} to be given back...", DisplayName(owner)));
                        }
                        auto next = req;
                        ++next.tries;
                        After(0.5f, [a_from, next]() { Apply(a_from, next); });
                        return;
                    }
                    if (auto mu = MuConflict(owner); !mu.empty()) {
                        Out(mu);
                        SetNotice(mu, true);
                        return;
                    }
                    const int r = ClaimReal(owner);
                    if (r < 0) {
                        SetNotice(std::format("{} could not take over (all {} real NPC entries are in use).", DisplayName(owner), kRealSlots), true);
                        return;
                    }
                    bool keep = false, preSummon = false;
                    {
                        std::lock_guard lock(g_lock);
                        keep = g_slots[a_slot].keep;
                        preSummon = g_slots[a_slot].preSummon;
                    }
                    slotName = DefaultName(a_slot);  // its own name (e.g. Cubbi), for the notice
                    Out(std::format("{} is given back; {} takes over (voice {:08X})", slotName, DisplayName(owner), req.voice));
                    TransferGear(old, owner);  // before the reset (worn list)
                    ResetSlot(a_slot);         // give back: own look, name, voice, outfit, sent home
                    {
                        std::lock_guard lock(g_lock);
                        g_slots[a_slot] = Slot{};
                        g_slots[a_slot].real = true;
                        g_slots[r].keep = keep;
                        g_slots[r].preSummon = preSummon;
                    }
                    a_slot = r;
                    takeOver = true;
                    switched = true;
                    g_selectMoved = true;
                    g_selectRequest = r;
                }
            }
            // NG 1.2 (user: the box disappeared after taking over, so it could not be undone): a real NPC with its
            // own modded voice (e.g. Sofia, taken over) and the box unticked: it is given back like Remove (own
            // look, name, outfit, sent to its own place) and a free Lazy Followers slot
            // becomes the follower with this preset, race and name and that voice (voice only, its mod's lines
            // swapped in). The follower's gear goes along (the take-over's TransferGear the other way).
            if (IsReal(a_slot) && !req.takeOver) {
                auto owner = ActorOf(a_slot);
                auto npc = owner ? owner->GetActorBase() : nullptr;
                if (npc && Voice::IsModded(npc->voiceType)) {
                    int freeSlot = -1;
                    {
                        std::lock_guard lock(g_lock);
                        for (int i = 0; i < g_lfCount; ++i) {
                            if (!g_slots[i].Listed()) {
                                freeSlot = i;
                                break;
                            }
                        }
                    }
                    auto slotActor = freeSlot >= 0 ? ActorOf(freeSlot) : nullptr;
                    if (!slotActor) {
                        SetNotice(std::format("All {} follower slots are in use - {} stays the follower.", g_lfCount, DisplayName(owner)), true);
                        return;
                    }
                    slotName = DisplayName(owner);
                    Out(std::format("{} is given back; follower slot {} gets its voice {:08X}", slotName, freeSlot + 1, req.voice));
                    TransferGear(owner, slotActor);
                    ResetSlot(a_slot);
                    {
                        std::lock_guard lock(g_lock);
                        g_slots[a_slot] = Slot{};
                        g_slots[a_slot].real = true;
                        g_slots[freeSlot].added = true;
                    }
                    a_slot = freeSlot;
                    gaveBack = true;
                    g_selectMoved = true;
                    g_selectRequest = freeSlot;
                }
            }
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            bool        erinChanged = false;
            std::string oldErin;  // the Erin preset it had
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                erinChanged = !s.erinPreset.empty() && s.erinPreset != req.erinPreset;
                oldErin = s.erinPreset;
                // taking over (user): the NPC gets the name typed for the follower as its nickname; the default
                // label "Follower <n>" (nothing typed) leaves its own name
                if (!req.name.empty() && !(takeOver && req.name == std::format("Follower {}", a_from + 1))) {
                    s.name = req.name;
                }
                if (!takeOver && Lookup<RE::BGSVoiceType>(req.voice)) {
                    s.voice = req.voice;  // taking over: the NPC keeps its own voice (NG 1.2: a real NPC's can be changed later)
                }
                if (Lookup<RE::TESRace>(req.race)) {
                    s.race = req.race;
                }
                s.preset = req.presetFile.substr(0, req.presetFile.size() - 6);  // the MCM's parsePreset
                s.headMesh = Files::HeadMeshFor(req.presetFile);
                s.erinPreset = req.erinPreset;
                if (req.keep) {
                    s.keep = true;
                    s.preSummon = req.summon;
                }
            }
            if (erinChanged) {
                ResetErin(actor, oldErin);  // NG: the old Erin preset's tints / skin / head-part positions / face shape off (another one or none now - e.g. not an Erin race any more)
            }
            // NG: the slot takes the preset's sex (from its head parts); unknown = the slot's current one
            const int sex = Files::PresetSex(req.presetFile);
            bool      female = false;
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                if (sex >= 0) {
                    s.female = sex == 1;
                }
                female = s.female;
            }
            ApplyBase(a_slot);
            // NG (user): Apply Settings also summons the follower (like Summon NPC) - right after the full apply
            // has finished (was a fixed Apply Delay + 1.5 s wait; user asked for faster). A disabled (dismissed)
            // follower is enabled first.
            auto start = [a_slot, summon = req.summon]() {
                if (auto a = ActorOf(a_slot); a && !IsReal(a_slot)) {
                    CheckRelationship(a);
                }
                if (!summon) {
                    // NG (user): a kept take-over without Pre Summon - applied where it is, not brought in
                    g_stayPut.insert(a_slot);
                    RefreshAppearance(a_slot, true);
                    return;
                }
                RefreshAppearance(a_slot, true, false, false, [a_slot]() {
                    // NG (user video 22-20-11: an Override follower 120 units away was still moved after the apply):
                    // a follower already shown next to the player stays where it is - only the usual lite pass
                    if (auto a = ActorOf(a_slot); a && !a->IsDisabled() && NextToPlayer(a)) {
                        Out(std::format("{} is already next to you - not summoned", DisplayName(a)));
                        ScheduleUpdate(a_slot, false, true);
                        return;
                    }
                    SummonUntilArrived(a_slot, 8);
                });
            };
            // NG (user): a preset applied with the wrong race could only be fixed with Remove (which
            // loses name, spells, stats...). Apply Settings' full pass now does Reset's appearance part first
            // (RefreshAppearance: clear the base NPC's RaceMenu preset, back to the slot's own race, wait),
            // then the picked race and preset as usual. Nothing else of the follower is reset.
            {
                std::lock_guard lock(g_lock);
                if (g_resetLook.size() < g_slots.size()) {
                    g_resetLook.resize(g_slots.size(), 0);
                    g_passes.resize(g_slots.size());
                }
                g_resetLook[a_slot] = 1;
            }
            if (actor->IsDisabled()) {
                auto chain = Papyrus::Chain::Make();
                chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "Enable", d, false); });
                chain->ThenDo([start]() { OnMain(start); });
                chain->Run();
            } else {
                start();
            }
            BuildLists();
            if (gaveBack) {
                SetNotice(std::format("{} was given back (own look and name); follower {} is now {} with {} and its voice.", slotName, a_slot + 1,
                    req.name.empty() ? DisplayName(actor) : req.name, req.presetFile));
                return;
            }
            if (switched) {
                SetNotice(std::format("{} was given back; {} (own voice and dialogue) is now {} with {}.", slotName, DefaultName(a_slot), DisplayName(actor),
                    req.presetFile));
                return;
            }
            if (takeOver) {
                SetNotice(std::format("{} (own voice and dialogue) is now {} with {} ({}); {} went back to storage.", DefaultName(a_slot), DisplayName(actor),
                    req.presetFile, female ? "female" : "male", slotName));
                return;
            }
            SetNotice(std::format("Applied {} to {} ({}{}).", req.presetFile, req.name.empty() ? DisplayName(actor) : req.name, female ? "female" : "male",
                sex < 0 ? ", sex not found in the preset - kept" : ""));
        });
    }

    void MoveOverride(int a_holder, int a_slot, ApplyRequest a_req)
    {
        OnMain([a_holder, a_slot, req = std::move(a_req)]() {
            ApplyRequest back;
            {
                std::lock_guard lock(g_lock);
                if (a_holder < 0 || a_holder >= static_cast<int>(g_slots.size()) || !g_slots[a_holder].real || !g_slots[a_holder].Active()) {
                    return;
                }
                const auto& h = g_slots[a_holder];
                back = { h.name, h.race, h.voice, h.preset + ".jslot", false };
                back.erinPreset = h.erinPreset;
            }
            Out(std::format("Override moves from {} to follower {}", back.name, a_slot + 1));
            Apply(a_holder, back);  // give back: the holder's follower goes to a free slot (voice only)
            auto take = req;
            take.takeOver = true;
            Apply(a_slot, take);  // waits for the give-back, then takes over
        });
    }

    void SetStyle(int a_slot, int a_style)
    {
        OnMain([a_slot, a_style]() {
            if (a_style < 0 || a_style >= kStyles || !g_styles[a_style]) {
                return;
            }
            {
                std::lock_guard lock(g_lock);
                g_slots[a_slot].style = g_styles[a_style]->GetFormID();
            }
            ApplyBase(a_slot);
            Out(std::format("Changed combatstyle for {}", g_slots[a_slot].name));
        });
    }

    void SetClass(int a_slot, RE::FormID a_class)
    {
        OnMain([a_slot, a_class]() {
            if (!Lookup<RE::TESClass>(a_class)) {
                return;
            }
            {
                std::lock_guard lock(g_lock);
                g_slots[a_slot].npcClass = a_class;
            }
            ApplyBase(a_slot);
            Out(std::format("Changed class for {}", g_slots[a_slot].name));
        });
    }

    void Summon(int a_slot)
    {
        OnMain([a_slot]() { SummonTo(ActorOf(a_slot), RE::PlayerCharacter::GetSingleton()); });
    }

    // NG (user): the Open Inventory hotkey - the actor ticked in Nearby Actors (Menu: a new console selection or a new
    // actor looked at is ticked first, see Menu::UpdatePick; user video 07-58-11: Vicky ticked but Ciel under the
    // crosshair opened Ciel's). An NPC's inventory opens like a follower's trade window (Papyrus
    // Actor.OpenInventory(true) - any NPC, take / give items); NG (user: "it should also work on the player") the player
    // ticked = the normal inventory (InventoryMenu). Not a missing / unloaded / dead NPC; a notification says why.
    // NG fix (user video 10-28-06: after Save Player Preset the camera still turned but the player could not walk;
    // opening + closing Modex freed the player). Found with Frida: the hotkey's key press also reached the game as
    // "Toggle POV", then our window took the input, so the game never saw the key go up. A held Toggle POV key =
    // PlayerControls data.fovSlideMode (the camera orbits, the player can't walk), and it stayed on. Movement controls,
    // input contexts and MovementHandler were all fine. Clearing fovSlideMode alone freed the player (tested live).
    // Called when one of our windows closes (main thread).
    void ResetPovHold()
    {
        OnMain([]() {
            auto pc = RE::PlayerControls::GetSingleton();
            if (pc && pc->data.fovSlideMode) {
                pc->data.fovSlideMode = false;
                logger::info("Toggle POV was still held after our window closed (its key release went to the window) - released");
            }
        });
    }

    std::atomic<RE::FormID> g_invActor{ 0 };  // NG: whose inventory Open Inventory opened last (0x14 = the player's)

    // The menu Open Inventory uses for a_ref: the player's own inventory, else the container (trade) menu.
    std::string_view InventoryMenuFor(RE::FormID a_ref) { return a_ref == 0x14 ? RE::InventoryMenu::MENU_NAME : RE::ContainerMenu::MENU_NAME; }

    RE::FormID InventoryOpenFor()
    {
        auto       ui = RE::UI::GetSingleton();
        const auto who = g_invActor.load();
        return ui && who && ui->IsMenuOpen(InventoryMenuFor(who)) ? who : 0;
    }

    void OpenInventory(RE::FormID a_ticked, int a_tries)
    {
        OnMain([a_ticked, a_tries]() {
            if (!InGame()) {
                return;
            }
            // NG (user video 08-19-05: another actor ticked in the Nearby window while the inventory was open did not
            // switch): the open inventory (an NPC's or yours) closes first, then the new one opens once it is gone (2 s max)
            auto ui = RE::UI::GetSingleton();
            const bool containerUp = ui && ui->IsMenuOpen(RE::ContainerMenu::MENU_NAME);
            const bool inventoryUp = ui && ui->IsMenuOpen(RE::InventoryMenu::MENU_NAME);
            if (containerUp || inventoryUp) {
                if (a_tries == 0) {
                    if (a_ticked == InventoryOpenFor()) {
                        // NG (user): the hotkey again on the same actor closes it (no reopen)
                        if (auto q = RE::UIMessageQueue::GetSingleton()) {
                            q->AddMessage(InventoryMenuFor(a_ticked), RE::UI_MESSAGE_TYPE::kHide, nullptr);
                        }
                        return;
                    }
                    if (auto q = RE::UIMessageQueue::GetSingleton()) {
                        q->AddMessage(containerUp ? RE::ContainerMenu::MENU_NAME : RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
                    }
                }
                if (a_tries < 20) {
                    After(0.1f, [a_ticked, a_tries]() { OpenInventory(a_ticked, a_tries + 1); });
                }
                return;
            }
            RE::Actor* actor = a_ticked ? RE::TESForm::LookupByID<RE::Actor>(a_ticked) : nullptr;
            if (actor && actor->IsPlayerRef()) {
                Out("Opening your inventory");
                g_invActor = 0x14;
                if (auto q = RE::UIMessageQueue::GetSingleton()) {
                    q->AddMessage(RE::InventoryMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kShow, nullptr);
                }
                return;
            }
            const char* why = nullptr;
            if (!actor) {
                why = "Lazy Followers: look at an NPC, select one in the console or tick one in Nearby Actors";
            } else if (actor->IsDead()) {
                why = "Lazy Followers: that NPC is dead - loot the body instead";
            } else if (!actor->Is3DLoaded()) {
                why = "Lazy Followers: the ticked NPC is not nearby";
            }
            if (why) {
                RE::DebugNotification(why);
                return;
            }
            Out(std::format("Opening {}'s inventory", DisplayName(actor)));
            g_invActor = actor->GetFormID();
            Papyrus::CallMethod(actor, "Actor", "OpenInventory", nullptr, true);
        });
    }

    // NG (user): the Summon hotkey. The follower selected in the pages (like the pages: an unlisted
    // selection falls back to the first listed follower); a notification says who comes or that none is set up.
    void SummonSelected()
    {
        OnMain([]() {
            if (!InGame()) {
                return;
            }
            int slot = -1;
            {
                std::lock_guard lock(g_lock);
                const int sel = g_snapSelected.load();
                if (sel >= 0 && sel < static_cast<int>(g_slots.size()) && g_slots[sel].Listed()) {
                    slot = sel;
                } else {
                    for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                        if (g_slots[i].Listed()) {
                            slot = i;
                            break;
                        }
                    }
                }
            }
            auto actor = slot >= 0 ? ActorOf(slot) : nullptr;
            if (!actor) {
                RE::DebugNotification("Lazy Followers: no follower to summon");
                return;
            }
            const auto text = std::format("Summoning {}", DisplayName(actor));
            Out(text);
            RE::DebugNotification(text.c_str());
            SummonTo(actor, RE::PlayerCharacter::GetSingleton());
        });
    }

    void TeleportTo(int a_slot)
    {
        OnMain([a_slot]() {
            auto actor = ActorOf(a_slot);
            auto player = RE::PlayerCharacter::GetSingleton();
            if (actor && player) {
                Papyrus::CallMethod(player, "ObjectReference", "MoveTo", {}, static_cast<RE::TESObjectREFR*>(actor), 0.0f, 0.0f, 0.0f, true);
            }
        });
    }

    // The actor script's dismissMe: reset, move to the storage cell marker, disable.
    void Dismiss(int a_slot)
    {
        OnMain([a_slot]() {
            auto actor = ActorOf(a_slot);
            if (IsReal(a_slot)) {
                // NG 1.2: a real NPC: own look and name back, entry freed; it stays where it is (no storage)
                ResetSlot(a_slot);
                {
                    std::lock_guard lock(g_lock);
                    g_slots[a_slot] = Slot{};
                    g_slots[a_slot].real = true;
                }
                BuildLists();
                SetNotice(std::format("{} was removed from the list (own look and name back, sent to its own place).", actor ? DisplayName(actor) : std::string("The NPC")));
                return;
            }
            if (!actor) {
                return;
            }
            StoreSlot(a_slot);
            BuildLists();
            SetNotice(std::format("{} was removed (reset and sent back to storage).", DisplayName(actor)));
        });
    }

    // The MCM's copyKit, NG (user: "it doesn't put it on the actor, and only copies equipped gear"): every item of the
    // player's inventory -> follower (playable, named, not gold or quest items; topped up to the player's count, so
    // pressing it again adds nothing new), added at once on the main thread; with Auto Equip on (Actions; the user's
    // was off - why nothing was put on) the follower then takes off what it wore and puts on the player's worn armor +
    // weapons (left-hand weapon / shield in the left hand). Items are copied as their base form: enchantments /
    // tempering made by the player are not.
    void CopyKit(int a_slot)
    {
        OnMain([a_slot]() {
            auto actor = ActorOf(a_slot);
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            auto       have = actor->GetInventoryCounts();
            const bool withWorn = IncludeEquipment();
            const auto wornList = WornGear(player);
            int        copied = 0;
            int        skipped = 0;
            for (auto& [item, data] : player->GetInventory()) {
                const auto count = data.first;
                if (!item || count <= 0 || item->IsGold()) {
                    continue;
                }
                if (!withWorn && std::find(wornList.begin(), wornList.end(), item) != wornList.end()) {
                    continue;  // Include Equipment off: what you wear stays yours alone
                }
                if (!item->GetPlayable() || !item->GetName() || !*item->GetName() || (data.second && data.second->IsQuestObject())) {
                    ++skipped;
                    continue;
                }
                const auto add = count - (have.contains(item) ? have[item] : 0);
                if (add > 0) {
                    actor->AddObjectToContainer(item, nullptr, add, nullptr);
                    copied += add;
                }
            }
            // Auto Equip: worn gear on - UnequipAll first, then each worn item of the player (now in the follower's inventory)
            auto leftObj = player->GetEquippedObject(true);
            auto chain = Papyrus::Chain::Make();
            int  worn = 0;
            const bool equip = AutoEquip() && withWorn;  // nothing worn copied = nothing to put on
            if (equip) {
                chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "UnequipAll", d); });
            }
            for (auto item : equip ? wornList : decltype(wornList){}) {
                if (!item->GetPlayable() || !item->GetName() || !*item->GetName()) {
                    continue;
                }
                const std::int32_t hand = item == leftObj ? 2 : 0;  // EquipItemEx slot: 2 = left hand, 0 = default
                chain->Then([actor, item, hand](Papyrus::Done d) {
                    return Papyrus::CallMethod(actor, "Actor", "EquipItemEx", d, static_cast<RE::TESForm*>(item), hand, false, true);
                });
                ++worn;
            }
            chain->Run();
            Out(std::format("CopyKit: {} items copied to {}, {} worn items put on, {} skipped (non playable / unnamed / quest)", copied,
                DisplayName(actor), worn, skipped));
            SetNotice(equip ? std::format("Copied {} items to {} and put on your {} worn items.", copied, DisplayName(actor), worn)
                                  : std::format("Copied {} items to {}{}.", copied, DisplayName(actor),
                                        withWorn ? " (tick Auto Equip to also put your worn gear on)" : " (not what you wear - Include Equipment is off)"));
        });
    }

    void RefreshAll()
    {
        OnMain([]() {
            int n = 0;
            for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active();
                }
                auto a = ActorOf(i);
                if (active && a && !a->IsDisabled()) {
                    // NG perf: followers far away (3D not loaded) get their full apply when they load
                    if (a->Is3DLoaded()) {
                        UpdateFollower(i, true);
                    } else {
                        g_fullOnLoad[i] = 1;
                    }
                    ++n;
                }
            }
            SetNotice(std::format("Full update started for {} followers (far away ones: when they load).", n));
        });
    }

    void SummonAll(bool a_notify)
    {
        OnMain([a_notify]() {
            if (!InGame()) {
                return;
            }
            int n = 0;
            for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active();
                }
                auto a = ActorOf(i);
                if (active && a && !Following(a)) {
                    ApplyBase(i);
                    Out(std::format("Summoning {}", DisplayName(a)));
                    SummonTo(a, RE::PlayerCharacter::GetSingleton());
                    ++n;
                }
            }
            SetNotice(std::format("Summoned {} followers.", n));
            if (a_notify) {
                const auto text = n > 0 ? std::format("Lazy Followers: summoning {} followers", n) : std::string("Lazy Followers: no followers to summon");
                RE::DebugNotification(text.c_str());
            }
        });
    }

    // The MCM's setNPCTravel: every unassigned follower goes to Follower Goes on a Trip (startWander),
    // summoned first; stops when FGT says its slots are full.
    void SendTravel()
    {
        OnMain([]() {
            if (!g_fgtQuest) {
                SetNotice("Follower Goes on a Trip (FGT.esp) is not installed.", true);
                return;
            }
            auto chain = Papyrus::Chain::Make();
            auto full = std::make_shared<std::atomic<bool>>(false);
            for (int i = static_cast<int>(g_actors.size()) - 1; i >= 0; --i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active() && !g_slots[i].real;  // real NPCs: their own AI, not sent on trips
                }
                auto actor = ActorOf(i);
                if (!active || !actor || Following(actor)) {
                    continue;
                }
                chain->Then([actor, full](Papyrus::Done d) {
                    if (full->load()) {
                        return false;
                    }
                    return Papyrus::CallMethod(g_fgtQuest, "FGTDialogueQuest", "startWander", [actor, full, d](const Papyrus::Variable& a_ok) {
                        if (a_ok.IsBool() && a_ok.GetBool()) {
                            Out(std::format("Travel sendoff for {}", DisplayName(actor)));
                            OnMain([actor]() { SummonTo(actor, RE::PlayerCharacter::GetSingleton()); });
                        } else {
                            Out(std::format("Travel sendoff for {} failed. All NPC slots full", DisplayName(actor)));
                            *full = true;
                        }
                        d(a_ok);
                    }, static_cast<RE::Actor*>(actor));
                });
            }
            chain->Run();
            SetNotice("Sending followers on a trip...");
        });
    }

    void ToggleSpell(int a_slot, RE::FormID a_spell)
    {
        OnMain([a_slot, a_spell]() {
            auto actor = ActorOf(a_slot);
            auto sp = Lookup<RE::SpellItem>(a_spell);
            if (!actor || !sp) {
                return;
            }
            bool had = false;
            {
                std::lock_guard lock(g_lock);
                auto&           list = g_slots[a_slot].spells;
                auto            it = std::find(list.begin(), list.end(), a_spell);
                had = it != list.end();
                if (had) {
                    list.erase(it);
                }
            }
            if (had) {
                actor->RemoveSpell(sp);
                Out(std::format("Removed spell {}", sp->GetName()));
            } else {
                AddSlotSpell(a_slot, sp);
                Out(std::format("Added spell {}", sp->GetName()));
            }
        });
    }

    void SetStat(int a_slot, const char* a_avName, float a_value)
    {
        std::string av = a_avName;
        OnMain([a_slot, av, a_value]() {
            auto actor = ActorOf(a_slot);
            if (!actor) {
                return;
            }
            {
                std::lock_guard lock(g_lock);
                auto&           d = g_slots[a_slot].statDefaults;
                if (!d.contains(av)) {
                    d[av] = actor->AsActorValueOwner()->GetPermanentActorValue(StatAV(av));  // the value before (Reset), without damage
                }
            }
            Papyrus::CallMethod(actor, "Actor", "ForceActorValue", {}, RE::BSFixedString(av), a_value);
        });
    }

    void ResetStat(int a_slot, const char* a_avName)
    {
        std::string av = a_avName;
        OnMain([a_slot, av]() { RestoreStats(a_slot, av); });
    }

    void ResetAllStats(int a_slot)
    {
        OnMain([a_slot]() {
            RestoreStats(a_slot);
            SetNotice("Stats and skills are back to their values before you changed them.");
        });
    }

    void LoadAllSpells()
    {
        OnMain([]() {
            const auto before = CurrentLists()->spells.size();
            {
                std::lock_guard lock(g_lock);
                g_allSpells = true;
            }
            BuildLists();
            SetNotice(std::format("Added {} spells.", CurrentLists()->spells.size() - before));
        });
    }

    // ==================================================================
    // combat styles
    // ==================================================================

    RE::TESCombatStyle* GetStyleForm(int a_index) { return a_index >= 0 && a_index < kStyles ? g_styles[a_index] : nullptr; }

    std::array<float, kStyleValues> StyleDefaults(int a_index)
    {
        return a_index >= 0 && a_index < kStyles ? g_styleDefaults[a_index] : std::array<float, kStyleValues>{};
    }

    // Same order as the MCM's getCombatStyleData (also the config file's order).
    std::array<float, kStyleValues> ReadStyle(const RE::TESCombatStyle* a_s)
    {
        std::array<float, kStyleValues> v{};
        if (!a_s) {
            return v;
        }
        const auto& g = a_s->generalData;
        const auto& m = a_s->meleeData;
        const auto& c = a_s->closeRangeData;
        v = { g.offensiveMult, g.defensiveMult, g.groupOffensiveMult, g.avoidThreatChance, g.meleeScoreMult, g.rangedScoreMult, g.magicScoreMult,
            g.shoutScoreMult, g.staffScoreMult, g.unarmedScoreMult, c.circleMult, c.fallbackMult, c.flankDistanceMult, c.stalkTimeMult,
            a_s->longRangeData.strafeMult, m.attackIncapacitatedMult, m.powerAttackIncapacitatedMult, m.powerAttackBlockingMult, m.bashMult,
            m.bashRecoilMult, m.bashAttackMult, m.bashPowerAttackMult, m.specialAttackMult,
            a_s->flags.all(RE::TESCombatStyle::FLAG::kAllowDualWielding) ? 1.0f : 0.0f };
        return v;
    }

    void WriteStyle(RE::TESCombatStyle* a_s, const std::array<float, kStyleValues>& v)
    {
        if (!a_s) {
            return;
        }
        auto& g = a_s->generalData;
        auto& m = a_s->meleeData;
        auto& c = a_s->closeRangeData;
        g.offensiveMult = v[0];
        g.defensiveMult = v[1];
        g.groupOffensiveMult = v[2];
        g.avoidThreatChance = v[3];
        g.meleeScoreMult = v[4];
        g.rangedScoreMult = v[5];
        g.magicScoreMult = v[6];
        g.shoutScoreMult = v[7];
        g.staffScoreMult = v[8];
        g.unarmedScoreMult = v[9];
        c.circleMult = v[10];
        c.fallbackMult = v[11];
        c.flankDistanceMult = v[12];
        c.stalkTimeMult = v[13];
        a_s->longRangeData.strafeMult = v[14];
        m.attackIncapacitatedMult = v[15];
        m.powerAttackIncapacitatedMult = v[16];
        m.powerAttackBlockingMult = v[17];
        m.bashMult = v[18];
        m.bashRecoilMult = v[19];
        m.bashAttackMult = v[20];
        m.bashPowerAttackMult = v[21];
        m.specialAttackMult = v[22];
        if (v[23] != 0.0f) {
            a_s->flags.set(RE::TESCombatStyle::FLAG::kAllowDualWielding);
        } else {
            a_s->flags.reset(RE::TESCombatStyle::FLAG::kAllowDualWielding);
        }
    }

    void SetStyleName(int a_index, std::string a_name)
    {
        if (a_index < 0 || a_index >= kStyles) {
            return;
        }
        std::lock_guard lock(g_lock);
        g_styleNames[a_index] = std::move(a_name);
    }

    void ResetStyle(int a_index)
    {
        if (a_index < 0 || a_index >= kStyles) {
            return;
        }
        WriteStyle(g_styles[a_index], g_styleDefaults[a_index]);
        std::lock_guard lock(g_lock);
        g_styleNames[a_index] = kStyleDefaultNames[a_index];
    }

    // ==================================================================
    // files
    // ==================================================================

    void SaveConfig()
    {
        OnMain([]() {
            Files::ConfigData cfg;
            auto              l = CurrentLists();
            cfg.femaleVoices = l->femaleVoices;
            cfg.maleVoices = l->maleVoices;
            cfg.races = l->races;
            {
                std::lock_guard lock(g_lock);
                cfg.styleNames = g_styleNames;
            }
            for (int i = 0; i < kStyles; ++i) {
                cfg.styles[i] = ReadStyle(g_styles[i]);
                cfg.hasStyle[i] = true;
            }
            if (Files::WriteConfig(cfg)) {
                SetNotice("Config saved to " + Files::ConfigFile().string());
            } else {
                SetNotice("Could not write " + Files::ConfigFile().string(), true);
            }
        });
    }

    void LoadConfig()
    {
        OnMain([]() {
            auto cfg = Files::ReadConfig();
            if (!cfg) {
                SetNotice("No readable " + Files::ConfigFile().string(), true);
                return;
            }
            {
                std::lock_guard lock(g_lock);
                for (auto& c : cfg->femaleVoices) {
                    AddChoice(g_extraFemaleVoices, c.id, c.name);
                }
                for (auto& c : cfg->maleVoices) {
                    AddChoice(g_extraMaleVoices, c.id, c.name);
                }
                for (auto& c : cfg->races) {
                    if (std::find(g_extraRaces.begin(), g_extraRaces.end(), c.id) == g_extraRaces.end()) {
                        g_extraRaces.push_back(c.id);
                    }
                }
                for (int i = 0; i < kStyles; ++i) {
                    if (cfg->hasStyle[i]) {
                        WriteStyle(g_styles[i], cfg->styles[i]);
                    }
                    if (!cfg->styleNames[i].empty()) {
                        g_styleNames[i] = cfg->styleNames[i];
                    }
                }
            }
            BuildLists();
            SetNotice("Config loaded.");
        });
    }

    void SaveAllNPCs()
    {
        OnMain([]() {
            nlohmann::json j = nlohmann::json::object();
            int            n = 0;
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active() && !g_slots[i].real;  // NG 1.2: real NPCs are not saved to NPC files
                }
                if (active) {
                    Out(std::format("Saving NPC - {}", g_slots[i].name));
                    j["NPC" + std::to_string(n++)] = Files::NPCToJson(ToNPCData(i), true);
                }
            }
            if (Files::WriteJson(Files::NPCFile(), j)) {
                SetNotice(std::format("Saved {} followers to {}", n, Files::NPCFile().string()));
            } else {
                SetNotice("Could not write " + Files::NPCFile().string(), true);
            }
        });
    }

    void SaveEachNPC()
    {
        OnMain([]() {
            int n = 0, failed = 0;
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                std::string name;
                {
                    std::lock_guard lock(g_lock);
                    if (!g_slots[i].Active() || g_slots[i].real) {
                        continue;  // NG 1.2: real NPCs are not saved to NPC files
                    }
                    name = g_slots[i].name;
                }
                const auto file = Files::SafeFileName(name);
                if (file.empty()) {
                    ++failed;
                    continue;
                }
                nlohmann::json j = nlohmann::json::object();
                j["NPC"] = Files::NPCToJson(ToNPCData(i), false);
                if (Files::WriteJson(Files::NPCFolder() / (file + ".json"), j)) {
                    ++n;
                } else {
                    ++failed;
                }
            }
            BuildLists();
            SetNotice(std::format("Saved {} followers to {}{}", n, Files::NPCFolder().string(), failed ? std::format(" ({} failed)", failed) : ""), failed > 0);
        });
    }

    // The ConfigManager's loadNPCData: every current follower is dismissed, then each "NPC<i>" goes to its
    // slotId. Slots that get loaded are not dismissed (their Disable could land after the load's Enable).
    void LoadAllNPCs()
    {
        OnMain([]() {
            auto j = Files::ReadJson(Files::NPCFile());
            if (!j) {
                SetNotice("No readable " + Files::NPCFile().string(), true);
                return;
            }
            std::vector<std::pair<int, Files::NPCData>> data;
            for (auto& [key, value] : j->items()) {
                auto n = Files::NPCFromJson(value);
                if (!n) {
                    continue;
                }
                int slot = n->slotId;
                if (slot < 0 || slot >= g_lfCount) {
                    slot = SlotOfActor(Lookup<RE::Actor>(n->actor));  // no slot id: the slot of the saved actor
                }
                if (slot >= 0 && slot < g_lfCount && std::none_of(data.begin(), data.end(), [&](const auto& p) { return p.first == slot; })) {
                    data.emplace_back(slot, std::move(*n));
                }
            }
            for (int i = 0; i < g_lfCount; ++i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active();
                }
                if (active && std::none_of(data.begin(), data.end(), [&](const auto& p) { return p.first == i; })) {
                    Dismiss(i);
                }
            }
            for (auto& [slot, n] : data) {
                ApplyNPCData(slot, n);
            }
            BuildLists();
            SetNotice(std::format("Loaded {} followers.", data.size()));
        });
    }

    // The MCM's loadSingleNPC: apply LazyFollowersNPCs/<file> to the selected slot, then summon. NG: the slot
    // takes the file's sex (the MCM refused a male / female mismatch).
    void LoadNPCPreset(int a_slot, std::string a_file)
    {
        OnMain([a_slot, file = std::move(a_file)]() {
            if (IsReal(a_slot)) {
                SetNotice("NPC presets are for Lazy Followers slots, not real NPCs.", true);
                return;
            }
            auto j = Files::ReadJson(Files::NPCFolder() / file);
            auto obj = j ? Files::Find(*j, "NPC") : nullptr;
            auto n = obj ? Files::NPCFromJson(*obj) : std::nullopt;
            if (!n) {
                SetNotice("Could not read " + file, true);
                return;
            }
            ApplyNPCData(a_slot, *n);
            SummonTo(ActorOf(a_slot), RE::PlayerCharacter::GetSingleton());
            BuildLists();
            SetNotice("Loaded " + file);
        });
    }

    // ==================================================================
    // Nearby Actors (Settings page) - Actor Control's scan: the player, then loaded actors in the radius
    // ==================================================================

    void ScanNearby()
    {
        if (!g_inGame.load() || g_scanning.exchange(true)) {
            return;
        }
        OnMain([]() {
            std::vector<NearbyActor> found;
            auto                     player = RE::PlayerCharacter::GetSingleton();
            auto                     lists = RE::ProcessLists::GetSingleton();
            if (player && lists) {
                constexpr float kUnitsPerMeter = 70.0f;  // same as Actor Control
                const float     radius = ScanRadius() * kUnitsPerMeter;
                const auto      origin = player->GetPosition();
                found.push_back({ player->GetFormID(), DisplayName(player), 0.0f });  // the player always first
                std::vector<NearbyActor> others;
                for (auto& handle : lists->highActorHandles) {
                    auto actor = handle.get();
                    if (!actor || actor.get() == player || actor->IsDead() || actor->IsDisabled()) {
                        continue;
                    }
                    const float d = origin.GetDistance(actor->GetPosition());
                    if (d <= radius) {
                        others.push_back({ actor->GetFormID(), DisplayName(actor.get()), d / kUnitsPerMeter });
                    }
                }
                std::sort(others.begin(), others.end(), [](const NearbyActor& a, const NearbyActor& b) { return a.meters < b.meters; });
                found.insert(found.end(), others.begin(), others.end());
            }
            {
                std::lock_guard lock(g_nearbyLock);
                g_nearby = std::move(found);
            }
            g_scanning = false;
        });
    }

    bool Scanning() { return g_scanning.load(); }

    std::vector<NearbyActor> Nearby()
    {
        std::lock_guard lock(g_nearbyLock);
        return g_nearby;
    }

    // The Nearby Actors pick sources right now (the hotkey can't wait for a snapshot): console selection, last
    // actor under the crosshair. Main thread (UI task).
    PickNow PickRefs() { return { IdOf(ConsoleActor()), IdOf(CrosshairActor()), ConsoleHasSelection() }; }

    void AddNearby(RE::FormID a_ref)
    {
        OnMain([a_ref]() {
            auto actor = Lookup<RE::Actor>(a_ref);
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            std::lock_guard lock(g_nearbyLock);
            if (std::none_of(g_nearby.begin(), g_nearby.end(), [&](const NearbyActor& n) { return n.id == a_ref; })) {
                g_nearby.push_back({ a_ref, DisplayName(actor), player->GetPosition().GetDistance(actor->GetPosition()) / 70.0f });
            }
        });
    }

    // ==================================================================
    // Save Actor Preset
    // ==================================================================

    void SavePreset(RE::FormID a_ref, std::string a_name, bool a_notify)
    {
        OnMain([a_ref, name = std::move(a_name), a_notify]() {
            RE::Actor* actor = Lookup<RE::Actor>(a_ref);
            // everything RaceMenu's console command forgot to check, plus a loaded head
            if (!actor) {
                SetNotice("No target actor - tick one in Nearby Actors.", true);
                return;
            }
            if (actor->IsDeleted() || !actor->GetActorBase()) {
                SetNotice("The target is not a valid actor.", true);
                return;
            }
            if (!actor->Is3DLoaded() || !actor->GetFaceNode()) {
                SetNotice(DisplayName(actor) + " is not loaded (needs to be near you, with a head).", true);
                return;
            }
            const auto file = Files::SafeFileName(name);
            if (file.empty()) {
                SetNotice("Type a preset name first.", true);
                return;
            }
            if (!Papyrus::HasFunction("CharGen", "SaveCharacterPreset")) {
                SetNotice("RaceMenu (CharGen script) not found.", true);
                return;
            }
            // CharGen.SaveCharacterPreset writes Data/SKSE/Plugins/CharGen/Presets/<name>.jslot; "../Exported/"
            // puts it where "skee preset-save" did and where Lazy Followers looks for presets.
            const auto path = Files::PresetFolder() / (file + ".jslot");
            std::error_code ec;
            std::filesystem::create_directories(Files::PresetFolder(), ec);
            const auto before = std::filesystem::exists(path, ec) ? std::filesystem::last_write_time(path, ec) : std::filesystem::file_time_type{};
            const std::string who = DisplayName(actor);
            const auto        handle = actor->GetHandle();
            // NG 1.2 (user: the player's tint .dds was never written - "the tint texture is not loaded"; the
            // player's face tint is not a plain texture). For the player use RaceMenu's own player save,
            // CharGen.SaveCharacter = what "skee preset-save" does (REA): SKSE\Plugins\CharGen\Exported\<name>.jslot
            // + a queued SKSETaskExportTintMask -> Textures\CharGen\Exported\<name>.dds. Takes the bare name.
            if (actor->IsPlayerRef() && Papyrus::HasFunction("CharGen", "SaveCharacter")) {
                const auto dds = std::filesystem::path("Data/Textures/CharGen/Exported") / (file + ".dds");
                const auto ddsBefore = std::filesystem::exists(dds, ec) ? std::filesystem::last_write_time(dds, ec) : std::filesystem::file_time_type{};
                const bool okPlayer = Papyrus::CallStatic("CharGen", "SaveCharacter", [path, before, dds, ddsBefore, who, file, a_notify](const Papyrus::Variable&) {
                    // the tint is exported by a queued RaceMenu task: look a moment later
                    After(1.5f, [path, before, dds, ddsBefore, who, file, a_notify]() {
                        std::error_code e;
                        const bool jslot = std::filesystem::exists(path, e) && std::filesystem::last_write_time(path, e) != before;
                        const bool tint = std::filesystem::exists(dds, e) && std::filesystem::last_write_time(dds, e) != ddsBefore;
                        const auto text = !jslot ? std::format("RaceMenu did not write {}.jslot - see skee64.log.", file) :
                                          tint   ? std::format("Saved {}'s preset as {}.jslot + {}.dds", who, file, file) :
                                                   std::format("Saved {}'s preset as {}.jslot (no tint .dds yet - see skee64.log)", who, file);
                        SetNotice(text, !jslot);
                        if (a_notify) {
                            RE::DebugNotification(text.c_str());
                        }
                        BuildLists();
                    });
                }, RE::BSFixedString(file));
                SetNotice(okPlayer ? "Saving preset..." : "Could not call RaceMenu (game not ready?).", !okPlayer);
                return;
            }
            const bool ok = Papyrus::CallStatic("CharGen", "SaveCharacterPreset", [path, before, who, file, handle](const Papyrus::Variable&) {
                std::error_code e;
                const bool      written = std::filesystem::exists(path, e) && std::filesystem::last_write_time(path, e) != before;
                if (!written) {
                    SetNotice(std::format("RaceMenu did not write {} - see skee64.log.", path.string()), true);
                    OnMain([]() { BuildLists(); });
                    return;
                }
                // then the face tint, like the console's Textures\CharGen\Exported\<name>.dds (main thread: D3D)
                OnMain([who, file, handle]() {
                    auto        ref = handle.get();
                    auto        target = ref ? ref->As<RE::Actor>() : nullptr;
                    std::string err;
                    const auto  dds = std::filesystem::path("Data/Textures/CharGen/Exported") / (file + ".dds");
                    if (target && Tint::ExportFaceTint(target, dds, err)) {
                        SetNotice(std::format("Saved {}'s preset as {}.jslot + {}.dds", who, file, file));
                    } else {
                        SetNotice(std::format("Saved {}'s preset as {}.jslot, but no tint .dds: {}", who, file, target ? err : "actor gone"), !target || !err.empty());
                    }
                    BuildLists();
                });
            }, actor, RE::BSFixedString("../Exported/" + file));
            if (!ok) {
                SetNotice("Could not call RaceMenu (game not ready?).", true);
            } else {
                SetNotice("Saving preset...");
            }
        });
    }
}
