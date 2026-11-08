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

#include "Files.h"
#include "Papyrus.h"
#include "Tint.h"

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

        // LazyFollowers.esp local form IDs (read from the esp with Tools/esp.py, 0.42.3).
        constexpr RE::FormID kNPCsList = 0x014F62;
        constexpr RE::FormID kStylesList = 0x014F5F;
        constexpr RE::FormID kFemaleVoicesList = 0x014F5D;
        constexpr RE::FormID kMaleVoicesList = 0x014F5E;
        constexpr RE::FormID kSpellsList = 0x033588;
        constexpr RE::FormID kClassesList = 0x0479B5;
        constexpr RE::FormID kFaction = 0x014F73;
        constexpr RE::FormID kCellMarker = 0x014F61;  // REFR of _LazyFollowersCellMarker in the storage cell
        constexpr std::array<RE::FormID, static_cast<int>(Global::kCount)> kGlobalIDs{ 0x060EF4, 0x060EF2, 0x060EF3, 0x060EF5, 0x0701F6 };
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
        std::vector<RE::Actor*>                 g_actors;  // the slots' actors (persistent references)
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
        bool g_allSpells = false;
        bool g_spellsEnabled = true;               // new (Settings): off greys out the Spells page
        float g_scanRadius = kDefaultScanRadius;   // Nearby Actors scan radius (metres)
        int   g_nearbyRows = kDefaultNearbyRows;   // Nearby Actors table rows before it scrolls
        std::vector<float>         g_nearbyWidths;  // Nearby Actors column widths (empty = default)
        std::atomic<std::uint64_t> g_openKey{ 0 };  // Open Window hotkey: vk | mods << 32 (read by the hotkey thread)
        std::shared_ptr<const Lists> g_lists = std::make_shared<Lists>();

        std::atomic<bool>     g_inGame{ false };
        std::atomic<unsigned> g_generation{ 0 };  // bumped on every load / revert: old delayed jobs stop
        std::atomic<int>      g_selectRequest{ -1 };

        std::mutex          g_crossLock;
        RE::ObjectRefHandle g_crosshair;

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

        // Log line; with Logging mode "Console" also printed to the console (the scripts' dout).
        void Out(const std::string& a_text)
        {
            logger::info("{}", a_text);
            if (g_globals[static_cast<int>(Global::kTrace)] && g_globals[static_cast<int>(Global::kTrace)]->value < 0.5f) {
                if (auto con = RE::ConsoleLog::GetSingleton()) {
                    con->Print("LazyFollowers: %s", a_text.c_str());
                }
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

        RE::Actor* ActorOf(int a_slot)
        {
            return a_slot >= 0 && a_slot < static_cast<int>(g_actors.size()) ? g_actors[a_slot] : nullptr;
        }

        RE::Actor* CrosshairActor()
        {
            std::lock_guard lock(g_crossLock);
            auto            ref = g_crosshair.get();
            return ref ? ref->As<RE::Actor>() : nullptr;
        }

        RE::Actor* ConsoleActor()
        {
            auto ref = RE::Console::GetSelectedRef();
            return ref ? ref->As<RE::Actor>() : nullptr;
        }

        std::string DisplayName(RE::TESObjectREFR* a_ref)
        {
            if (!a_ref) {
                return {};
            }
            const char* n = a_ref->GetDisplayFullName();
            return n && *n ? n : std::format("{:08X}", a_ref->GetFormID());
        }

        int SlotOfActor(const RE::Actor* a_actor)
        {
            for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                if (g_actors[i] && g_actors[i] == a_actor) {
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
            if (!npc) {
                return;
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
            if (auto v = Lookup<RE::BGSVoiceType>(s.Active() ? s.voice : s.defaultVoice)) {
                npc->voiceType = v;
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
            }
            if (!s.Active()) {
                return;
            }
            auto race = Lookup<RE::TESRace>(s.race);
            if (!race) {
                race = actor->GetRace();
            }
            // NG (user): one RaceMenu Mode for every pass (the esp's RMLiteMode global; RMFullMode is unused)
            const auto fullMode = std::int32_t(GetGlobal(Global::kLiteMode));
            const auto liteMode = std::int32_t(a_body ? fullMode : (fullMode & ~2));
            const RE::BSFixedString preset(s.preset);
            const bool              head = !s.headMesh.empty();
            auto                    hair = g_hairColor;

            auto chain = Papyrus::Chain::Make();
            if (actor->GetRace() != race) {
                chain->Then([actor, race](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "SetRace", d, race); });
            }
            const std::string what = a_full ? (head ? "Headmesh Full" : "Full") : (head ? "HeadMesh Lite" : "Lite");
            chain->ThenDo([what, name = s.name, p = s.preset]() { Out(std::format("{} - applying preset - {} - {}", name, what, p)); });
            if (a_full) {
                if (head) {
                    chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadCharacterPresetEx", d, actor, preset, hair, fullMode); });
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
            if (a_full || a_body) {
                chain->Then([=](Papyrus::Done d) { return Papyrus::CallStatic("CharGen", "LoadCharacterEx", d, actor, race, preset, std::int32_t(2)); });
            }
            // SetRace / the preset can drop the display name: set it again at the end
            chain->ThenDo([a_slot]() { OnMain([a_slot]() { ApplyName(a_slot); }); });
            // Every LoadCharacterEx resets the 3D, and full mode (0) has no overlay / skin-texture bits (1, 8).
            // In the old mod that reset fired the script's OnLoad, which then did the lite apply (11 = overlays
            // + morphs + skin) - that is what gave the face its textures. Our load event does not fire on a 3D
            // reset, so do that OnLoad pass here.
            if (a_full) {
                chain->ThenDo([a_slot]() { OnMain([a_slot]() { ScheduleUpdate(a_slot, false, true, true); }); });
            } else if (a_playerFace) {
                chain->ThenDo([]() { OnMain([]() { RefreshPlayerFace(); }); });
            }
            if (a_then) {
                chain->ThenDo([a_then]() { OnMain(a_then); });
            }
            chain->Run();
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
            CheckRelationship(actor);
            RefreshAppearance(a_slot, a_full, a_playerFace, a_body);
        }

        // OnLoad of the actor script: wait "Apply delay" seconds, then update.
        void ScheduleUpdate(int a_slot, bool a_full, bool a_playerFace, bool a_body)
        {
            After(static_cast<float>(GetGlobal(Global::kDelay)), [a_slot, a_full, a_playerFace, a_body]() { UpdateFollower(a_slot, a_full, a_playerFace, a_body); });
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

        // The scripts' resetMyData: back to the esp's name / voice / race / style / class, no preset, no
        // spells, RaceMenu preset cleared. (The old script kept the preset race on the actor; we switch back.)
        void ResetSlot(int a_slot)
        {
            auto actor = ActorOf(a_slot);
            RemoveSlotSpells(a_slot);
            RestoreStats(a_slot);
            RE::FormID defRace = 0;
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                s.name = s.defaultName;
                s.preset.clear();
                s.headMesh.clear();
                s.race = s.defaultRace;
                s.voice = s.defaultVoice;
                s.style = s.defaultStyle;
                s.npcClass = s.defaultClass;
                s.female = s.defaultFemale;
                s.added = false;  // out of the Followers list
                defRace = s.defaultRace;
            }
            ApplyBase(a_slot);
            if (actor) {
                if (auto npc = actor->GetActorBase()) {
                    Papyrus::CallStatic("CharGen", "ClearPreset", {}, npc);
                }
                if (auto r = Lookup<RE::TESRace>(defRace); r && actor->GetRace() != r) {
                    Papyrus::CallMethod(actor, "Actor", "SetRace", {}, r);
                }
            }
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
            auto vanilla = dh ? dh->LookupForm<RE::BGSListForm>(0x014F72, kPlugin) : nullptr;  // _LazyFollowersRaces
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
            s.crosshairRef = IdOf(CrosshairActor());
            {
                std::lock_guard lock(g_lock);
                s.slots = g_slots;
                s.styleNames = g_styleNames;
                s.lists = g_lists;
            }
            s.live.resize(g_actors.size());
            for (std::size_t i = 0; i < g_actors.size(); ++i) {
                if (auto a = g_actors[i]) {
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
        constexpr std::uint32_t kVersion = 1;

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

        // Reads a form ID and maps it to this load order (0 if its plugin is gone).
        RE::FormID ReadForm(SKSE::SerializationInterface* a_intfc)
        {
            RE::FormID old = 0, now = 0;
            a_intfc->ReadRecordData(old);
            if (old && a_intfc->ResolveFormID(old, now)) {
                return now;
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
                s.name = s.defaultName;
                s.preset.clear();
                s.headMesh.clear();
                s.race = s.defaultRace;
                s.voice = s.defaultVoice;
                s.style = s.defaultStyle;
                s.npcClass = s.defaultClass;
                s.female = s.defaultFemale;
                s.added = false;
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
        constexpr std::array<const char*, static_cast<int>(Global::kCount)> kGlobalKeys{ "ApplyDelay", "RMFullMode", "RMLiteMode",
            "LoggingMode", "Sandboxing" };

        // Current Settings page values -> the file.
        void SaveSettings()
        {
            nlohmann::json j = nlohmann::json::object();
            for (int i = 0; i < static_cast<int>(Global::kCount); ++i) {
                j[kGlobalKeys[i]] = GetGlobal(static_cast<Global>(i));
            }
            {
                std::lock_guard lock(g_lock);
                j["CustomCombatStyles"] = g_stylesEnabled;
                j["CustomClasses"] = g_classesEnabled;
                j["AutoEquip"] = g_autoEquip;
                j["CustomSpells"] = g_spellsEnabled;
                j["ScanRadius"] = g_scanRadius;
                j["NearbyRows"] = g_nearbyRows;
                j["NearbyWidths"] = g_nearbyWidths;
            }
            const auto key = OpenWindowKey();
            j["OpenWindowKey"] = { { "VirtualKey", key.vk }, { "Mods", key.mods } };
            Files::WriteJson(Files::SettingsFile(), j);
        }

        // The settings that need no game: Open Window hotkey and the Nearby Actors column widths.
        void LoadUiSettings(const nlohmann::json& a_j)
        {
            if (auto v = Files::Find(a_j, "OpenWindowKey"); v && v->is_object()) {
                KeyBinding k;
                if (auto vk = Files::Find(*v, "VirtualKey"); vk && vk->is_number_unsigned()) {
                    k.vk = std::min<std::uint32_t>(vk->get<std::uint32_t>(), 255);
                }
                if (auto m = Files::Find(*v, "Mods"); m && m->is_number_unsigned()) {
                    k.mods = m->get<std::uint32_t>() & (kModCtrl | kModShift | kModAlt);
                }
                g_openKey = static_cast<std::uint64_t>(k.vk) | (static_cast<std::uint64_t>(k.mods) << 32);
            }
            if (auto v = Files::Find(a_j, "NearbyWidths"); v && v->is_array()) {
                std::vector<float> w;
                for (auto& x : *v) {
                    if (x.is_number()) {
                        w.push_back(std::clamp(x.get<float>(), 20.0f, 2000.0f));
                    }
                }
                std::lock_guard lock(g_lock);
                g_nearbyWidths = std::move(w);
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
                if (auto v = Files::Find(*j, kGlobalKeys[i]); v && v->is_number()) {
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
                if (version != kVersion) {
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
            }
        }

        // ---- events ----

        // Crosshair event sink: keeps the last reference under the crosshair (none when it leaves).
        class CrosshairSink : public RE::BSTEventSink<SKSE::CrosshairRefEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const SKSE::CrosshairRefEvent* a_event, RE::BSTEventSource<SKSE::CrosshairRefEvent>*) override
            {
                std::lock_guard lock(g_crossLock);
                g_crosshair = a_event && a_event->crosshairRef ? a_event->crosshairRef->GetHandle() : RE::ObjectRefHandle{};
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
                for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                    if (g_actors[i] && g_actors[i]->GetFormID() == a_event->formID) {
                        if (i < static_cast<int>(g_fullOnLoad.size()) && g_fullOnLoad[i]) {
                            g_fullOnLoad[i] = 0;
                            ScheduleUpdate(i, true);  // Apply to All while it was not loaded
                        } else {
                            ScheduleUpdate(i, false);
                        }
                        break;
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
                for (int i = 0; ref && i < static_cast<int>(g_actors.size()); ++i) {
                    if (g_actors[i] == ref) {
                        OnMain([i]() { ApplyName(i); });  // ApplyName only writes when the name differs
                        break;
                    }
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

        // The ConfigManager's loadNPCActorVars + loadNPCSpellVars + loadNPCGear for one slot.
        void ApplyNPCData(int a_slot, const Files::NPCData& a_n)
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
                chain->Then([actor, item](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "EquipItemEx", d, item, std::int32_t(0), false, true); });
            }
            chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "QueueNiNodeUpdate", d); });
            chain->ThenDo([a_slot]() { OnMain([a_slot]() { RefreshAppearance(a_slot, true); }); });
            chain->Run();
        }
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
        g_formsFound = !g_slots.empty();
        logger::info("{}: {} follower slots, RaceMenu hair colour {}, Follower Goes on a Trip {}", kPlugin, g_slots.size(), g_hairColor ? "found" : "missing",
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
    }

    void RegisterCosave()
    {
        auto s = SKSE::GetSerializationInterface();
        s->SetUniqueID(kCosaveID);
        s->SetSaveCallback(OnSave);
        s->SetLoadCallback(OnLoad);
        s->SetRevertCallback(OnRevert);
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
        LoadSettings();  // global settings over the save's (before ApplyBase: combat styles / classes on or off)
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
            if (active && g_actors[i] && g_actors[i]->Is3DLoaded()) {
                ScheduleUpdate(i, false);
            }
        }
    }

    void OnLeaveGame()
    {
        g_inGame = false;
        ++g_generation;
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

    int TakeSelect() { return g_selectRequest.exchange(-1); }

    unsigned GameLoads() { return g_generation.load(); }

    // The first slot not in the Followers list joins it (no preset yet) and gets selected.
    void AddFollower()
    {
        OnMain([]() {
            int slot = -1;
            {
                std::lock_guard lock(g_lock);
                for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
                    if (!g_slots[i].Listed()) {
                        g_slots[i].added = true;
                        slot = i;
                        break;
                    }
                }
            }
            if (slot < 0) {
                SetNotice(std::format("All {} follower slots are in use.", g_slots.size()), true);
                return;
            }
            g_selectRequest = slot;
            SetNotice(std::format("Follower {} added - pick a preset and Apply Settings.", slot + 1));
        });
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

    void SetGlobal(Global a_g, int a_value)
    {
        if (auto g = g_globals[static_cast<int>(a_g)]; g && g->value != static_cast<float>(a_value)) {
            g->value = static_cast<float>(a_value);
            SaveSettings();
        }
    }

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
            g_scanRadius = kDefaultScanRadius;
            g_nearbyRows = kDefaultNearbyRows;
        }
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

    void Apply(int a_slot, ApplyRequest a_req)
    {
        OnMain([a_slot, req = std::move(a_req)]() {
            auto actor = ActorOf(a_slot);
            if (!actor || req.presetFile.size() <= 6) {
                SetNotice("Pick a preset first.", true);
                return;
            }
            {
                std::lock_guard lock(g_lock);
                auto&           s = g_slots[a_slot];
                if (!req.name.empty()) {
                    s.name = req.name;
                }
                if (Lookup<RE::BGSVoiceType>(req.voice)) {
                    s.voice = req.voice;
                }
                if (Lookup<RE::TESRace>(req.race)) {
                    s.race = req.race;
                }
                s.preset = req.presetFile.substr(0, req.presetFile.size() - 6);  // the MCM's parsePreset
                s.headMesh = Files::HeadMeshFor(req.presetFile);
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
            auto start = [a_slot]() {
                if (auto a = ActorOf(a_slot)) {
                    CheckRelationship(a);
                }
                RefreshAppearance(a_slot, true, false, false, [a_slot]() { SummonUntilArrived(a_slot, 8); });
            };
            if (actor->IsDisabled()) {
                auto chain = Papyrus::Chain::Make();
                chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "Enable", d, false); });
                chain->ThenDo([start]() { OnMain(start); });
                chain->Run();
            } else {
                start();
            }
            BuildLists();
            SetNotice(std::format("Applied {} to {} ({}{}).", req.presetFile, req.name.empty() ? DisplayName(actor) : req.name, female ? "female" : "male",
                sex < 0 ? ", sex not found in the preset - kept" : ""));
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
            if (!actor) {
                return;
            }
            ResetSlot(a_slot);
            auto chain = Papyrus::Chain::Make();
            if (g_marker) {
                chain->Then([actor](Papyrus::Done d) {
                    return Papyrus::CallMethod(actor, "ObjectReference", "MoveTo", d, g_marker, 0.0f, 0.0f, 0.0f, true);
                });
            }
            chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "Disable", d, false); });
            chain->Run();
            BuildLists();
            SetNotice(std::format("{} was reset and sent back to storage.", DisplayName(actor)));
        });
    }

    // The MCM's copyKit: the player's worn armor + weapons -> follower (playable, named, not already owned);
    // equipped too when Auto Equip is on (left-hand weapon / shield in the left hand).
    void CopyKit(int a_slot)
    {
        OnMain([a_slot]() {
            auto actor = ActorOf(a_slot);
            auto player = RE::PlayerCharacter::GetSingleton();
            if (!actor || !player) {
                return;
            }
            const bool equip = AutoEquip();
            auto       counts = actor->GetInventoryCounts();
            auto       leftObj = player->GetEquippedObject(true);
            auto       chain = Papyrus::Chain::Make();
            chain->Then([actor](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "UnequipAll", d); });
            int copied = 0;
            for (auto item : WornGear(player)) {
                if (!item->GetPlayable()) {
                    Out(std::format("CopyKit: item {:08X} flagged as non playable, skipped", item->GetFormID()));
                    continue;
                }
                if (counts.contains(item) && counts[item] > 0) {
                    Out(std::format("CopyKit: item {} already in the follower's inventory, skipped", item->GetName()));
                    continue;
                }
                if (!item->GetName() || !*item->GetName()) {
                    Out(std::format("CopyKit: item {:08X} has no name, skipped", item->GetFormID()));
                    continue;
                }
                const bool left = item == leftObj;
                chain->Then([actor, item](Papyrus::Done d) { return Papyrus::CallMethod(actor, "ObjectReference", "AddItem", d, static_cast<RE::TESForm*>(item), std::int32_t(1), true); });
                if (equip) {
                    if (left) {
                        chain->Then([actor, item](Papyrus::Done d) {
                            return Papyrus::CallMethod(actor, "Actor", "EquipItemEx", d, static_cast<RE::TESForm*>(item), std::int32_t(2), false, true);
                        });
                    } else {
                        chain->Then([actor, item](Papyrus::Done d) { return Papyrus::CallMethod(actor, "Actor", "EquipItem", d, static_cast<RE::TESForm*>(item), false, false); });
                    }
                }
                ++copied;
            }
            chain->Run();
            SetNotice(std::format("Equipment copied to {} ({} items).", DisplayName(actor), copied));
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
                if (active && g_actors[i] && !g_actors[i]->IsDisabled()) {
                    // NG perf: followers far away (3D not loaded) get their full apply when they load
                    if (g_actors[i]->Is3DLoaded()) {
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

    void SummonAll()
    {
        OnMain([]() {
            int n = 0;
            for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                bool active = false;
                {
                    std::lock_guard lock(g_lock);
                    active = g_slots[i].Active();
                }
                if (active && !Following(g_actors[i])) {
                    ApplyBase(i);
                    Out(std::format("Summoning {}", DisplayName(g_actors[i])));
                    SummonTo(g_actors[i], RE::PlayerCharacter::GetSingleton());
                    ++n;
                }
            }
            SetNotice(std::format("Summoned {} followers.", n));
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
                    active = g_slots[i].Active();
                }
                auto actor = g_actors[i];
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
                    active = g_slots[i].Active();
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
                    if (!g_slots[i].Active()) {
                        continue;
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
                if (slot < 0 || slot >= static_cast<int>(g_slots.size())) {
                    slot = SlotOfActor(Lookup<RE::Actor>(n->actor));  // no slot id: the slot of the saved actor
                }
                if (slot >= 0 && std::none_of(data.begin(), data.end(), [&](const auto& p) { return p.first == slot; })) {
                    data.emplace_back(slot, std::move(*n));
                }
            }
            for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
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

    void SavePreset(RE::FormID a_ref, std::string a_name)
    {
        OnMain([a_ref, name = std::move(a_name)]() {
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
