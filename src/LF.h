#pragma once

// LF - Lazy Followers' logic, moved from its Papyrus scripts (_LazyFollowersActorScript, _MCMScript,
// _ConfigManager, _PlayerScript) into C++. LazyFollowers.esp still provides the follower slots (100
// since NG 1.1: Tools/add_slots.py added 20 to the FLST),
// form lists, globals, faction, sandbox package and storage cell; its scripts are stripped.
// State that the scripts kept in the save (preset, name, race, voice...) is kept in the SKSE co-save.
// Threads: the menu (render thread) reads a Snapshot and queues actions; all game work runs on the main
// thread (SKSE tasks) or the Papyrus VM (Papyrus.h).

namespace LF
{
    constexpr int kStyles = 10;       // _LazyFollowersCombatStyles: LazyFollowersCS1..CS10
    constexpr int kStyleValues = 24;  // values per combat style (same order as the MCM's getCombatStyleData)

    // One entry of a drop-down list: a form and its display name. group = the spell's magic school (Spells
    // page folders, see kSpellSchools), 0 for other lists.
    struct Choice
    {
        RE::FormID  id = 0;
        std::string name;
        int         group = 0;
    };

    // Spells page folders: the school of a spell's costliest effect; the last one = everything else.
    constexpr std::array<const char*, 6> kSpellSchools{ "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Other" };

    // A follower slot (one of the 100 actors). Empty preset = unused slot, like the scripts' myPreset == "".
    // female = the sex in use (NG: set from the applied preset, any slot can be male or female);
    // defaultFemale = the esp's, restored by Reset / Dismiss.
    struct Slot
    {
        RE::FormID  ref = 0;         // the slot's actor reference (ACHR in LazyFollowers.esp)
        RE::FormID  base = 0;        // its base NPC (shown in the Follower drop-down)
        std::string defaultName;     // the esp values, restored by Reset / Dismiss
        RE::FormID  defaultRace = 0;
        RE::FormID  defaultVoice = 0;
        RE::FormID  defaultStyle = 0;
        RE::FormID  defaultClass = 0;
        bool        defaultFemale = false;
        bool        female = false;

        std::string             name;      // preset name = the follower's name
        std::string             preset;    // RaceMenu preset, "../Exported/<file without .jslot>"
        std::string             headMesh;  // "" or the preset's .nif (then RaceMenu's head-mesh functions are used)
        RE::FormID              race = 0;
        RE::FormID              voice = 0;
        RE::FormID              style = 0;
        RE::FormID              npcClass = 0;
        std::vector<RE::FormID> spells;
        bool                    added = false;  // NG: in the Followers list (Add Follower) but no preset applied yet
        std::map<std::string, float> statDefaults;  // NG: Stats page values changed -> their value before (Reset)

        bool Active() const { return !preset.empty(); }
        bool Listed() const { return Active() || added; }  // shown in the Follower drop-down
    };

    // What the menu shows about a slot right now (read on the main thread).
    struct SlotLive
    {
        bool enabled = false;    // not disabled (= not in storage)
        bool loaded = false;     // 3D loaded (near the player)
        bool following = false;  // CurrentFollowerFaction rank >= 1
    };

    // Lists for the drop-downs, rebuilt when a game loads and when the Followers page opens.
    struct Lists
    {
        std::vector<Choice>      races;         // every playable race in the load order + races met
        std::vector<Choice>      femaleVoices;  // _LazyFollowersFemaleVoices + voices met
        std::vector<Choice>      maleVoices;
        std::vector<Choice>      otherVoices;   // NG: every other voice type of the load order (modded voices...)
        std::vector<Choice>      classes;       // _LazyFollowersClasses
        std::vector<Choice>      spells;        // _LazyFollowersSpells (+ all playable spells when loaded)
        std::vector<std::string> presets;       // Data/SKSE/Plugins/CharGen/Exported/*.jslot
        std::vector<std::string> npcPresets;    // Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs/*.json
    };

    constexpr int kStatCount = 12;   // WeaponSpeedMult ... MagickaRate (Stats page, left column)
    constexpr int kSkillCount = 18;  // the MCM's skillNames

    struct Snapshot
    {
        bool                         valid = false;
        std::vector<Slot>            slots;
        std::vector<SlotLive>        live;
        std::shared_ptr<const Lists> lists;
        std::array<std::string, kStyles> styleNames;
        std::array<float, kStatCount>    stats{};   // the selected slot's actor values
        std::array<float, kSkillCount>   skills{};
        float       selDistance = -1.0f;  // NG: the selected follower's distance to you in metres (-1 = not near you)
        std::string selLocation;          // NG: its location / cell / worldspace ("" = unknown)
        RE::FormID  consoleRef = 0;    // console-selected actor (0 = none)
        RE::FormID  crosshairRef = 0;  // last actor under the crosshair (0 = none)
        bool        raceMenu = false;  // CharGen script found (RaceMenu installed)
        bool        fgt = false;       // Follower Goes on a Trip installed
        bool        po3 = false;
    };

    // One actor of the Settings page's Nearby Actors table (Actor Control's scan): the player first, then
    // loaded actors within the scan radius, nearest first.
    struct NearbyActor
    {
        RE::FormID  id = 0;
        std::string name;
        float       meters = 0.0f;
    };

    // ---- setup (main.cpp) ----
    void LoadForms();          // kDataLoaded
    bool FormsFound();
    void InstallSinks();       // kDataLoaded: crosshair + 3D loaded events
    void RegisterCosave();     // SKSEPluginLoad
    void OnGameLoaded();       // kPostLoadGame / kNewGame
    void OnLeaveGame();        // kPreLoadGame
    bool InGame();

    // ---- menu ----
    void                     RequestSnapshot(int a_selected);
    std::shared_ptr<const Snapshot> GetSnapshot();     // the latest snapshot (rebuilt at most ~10 times a second)
    int                      SlotCount();
    void                     OnFollowersPageOpened();  // the MCM's OnConfigOpen: crosshair NPC -> select / add race + voice
    int                      TakeSelect();             // slot to select (from OnFollowersPageOpened / a load), -1 = none
    unsigned                 GameLoads();              // counts loads / new games (menu: forget unapplied edits)
    void                     AddFollower();            // NG: first unlisted slot -> Followers list, selected
    void                     RebuildLists();
    std::string              Notice(bool& a_error);
    void                     ClearNotice();
    void                     SetNotice(std::string a_text, bool a_error = false);

    // ---- settings (globals live in the esp + save; the rest in the co-save) ----
    enum class Global
    {
        kDelay,     // _LazyFollowersRMDelay: seconds after an NPC loads before the preset is applied
        kFullMode,  // _LazyFollowersRMFullMode: no longer used (NG: merged into RaceMenu Mode, user)
        kLiteMode,  // _LazyFollowersRMLiteMode: "RaceMenu Mode" - flags for every preset apply
        kTrace,     // _LazyFollowersTraceMode: 0 console, 1 log file, 2 off
        kSandbox,   // _LazyFollowersSandboxEnable: idle followers sandbox (package condition)
        kCount
    };
    int  GetGlobal(Global a_g);
    void SetGlobal(Global a_g, int a_value);
    int  GlobalDefault(Global a_g);

    bool StylesEnabled();
    void SetStylesEnabled(bool a_on);
    bool ClassesEnabled();
    void SetClassesEnabled(bool a_on);
    bool SpellsEnabled();      // new: off greys out the Spells page (spells already added stay)
    void SetSpellsEnabled(bool a_on);
    bool AutoEquip();
    void SetAutoEquip(bool a_on);
    bool AllSpellsLoaded();
    constexpr float kDefaultScanRadius = 20.0f;
    constexpr int   kDefaultNearbyRows = 10;
    float ScanRadius();        // Nearby Actors scan radius in metres
    void  SetScanRadius(float a_meters);
    int   NearbyRows();        // Nearby Actors table: rows before it scrolls (its grip)
    void  SetNearbyRows(int a_rows);
    void  ResetSettings();     // Settings page "Reset All": every Settings value back to its default (not the hotkey)
    std::vector<float> NearbyWidths();  // Nearby Actors table column widths as the user dragged them (empty = default)
    void               SetNearbyWidths(const std::vector<float>& a_widths);

    // NG 1.1 (user): the Settings page's hotkey that opens / closes the Lazy Followers window. vk = Windows
    // virtual key (0 = none), mods = Ctrl / Shift / Alt that must be held with it.
    constexpr std::uint32_t kModCtrl = 1;
    constexpr std::uint32_t kModShift = 2;
    constexpr std::uint32_t kModAlt = 4;
    struct KeyBinding
    {
        std::uint32_t vk = 0;
        std::uint32_t mods = 0;
        bool          IsSet() const { return vk != 0; }
    };
    KeyBinding OpenWindowKey();  // any thread
    void       SetOpenWindowKey(KeyBinding a_key);
    void       LoadEarlySettings();  // kPostLoad: the hotkey + table widths from the settings file (no game needed)

    // ---- follower actions (queued to the main thread) ----
    struct ApplyRequest
    {
        std::string name;
        RE::FormID  race = 0;
        RE::FormID  voice = 0;
        std::string presetFile;  // "<name>.jslot"
    };
    void Apply(int a_slot, ApplyRequest a_req);    // the MCM's "Apply settings" (full preset apply)
    void SetStyle(int a_slot, int a_style);
    void SetClass(int a_slot, RE::FormID a_class);
    void Summon(int a_slot);
    void TeleportTo(int a_slot);
    void Dismiss(int a_slot);                      // reset + back to storage
    void CopyKit(int a_slot);                      // player's worn gear -> follower
    void RefreshAll();                             // "Apply to all": full preset re-apply on every follower
    void SummonAll();
    void SendTravel();                             // Follower Goes on a Trip
    void ToggleSpell(int a_slot, RE::FormID a_spell);
    void SetStat(int a_slot, const char* a_avName, float a_value);  // first change remembers the value before
    void ResetStat(int a_slot, const char* a_avName);               // back to the remembered value
    void ResetAllStats(int a_slot);
    void LoadAllSpells();

    // ---- combat styles (forms edited directly from the menu, like the MCM did) ----
    RE::TESCombatStyle*        GetStyleForm(int a_index);
    std::array<float, kStyleValues> StyleDefaults(int a_index);  // the esp's values (Reset buttons)
    std::array<float, kStyleValues> ReadStyle(const RE::TESCombatStyle* a_style);
    void                       WriteStyle(RE::TESCombatStyle* a_style, const std::array<float, kStyleValues>& a_v);
    void                       SetStyleName(int a_index, std::string a_name);
    void                       ResetStyle(int a_index);  // Combat Styles "Reset All": esp values + default name

    // ---- files (Data/SKSE/Plugins/LazyFollowers, same files and layout as the old mod) ----
    void SaveConfig();
    void LoadConfig();
    void SaveAllNPCs();        // all followers in LazyFollowersNPC.json
    void SaveEachNPC();        // one LazyFollowersNPCs/<name>.json per follower
    void LoadAllNPCs();        // LazyFollowersNPC.json -> slots (current followers are dismissed first)
    void LoadNPCPreset(int a_slot, std::string a_file);

    // ---- Nearby Actors (Settings page; like Actor Control's) ----
    void                     ScanNearby();                 // queued to the main thread; results arrive later
    bool                     Scanning();
    std::vector<NearbyActor> Nearby();
    void                     AddNearby(RE::FormID a_ref);  // console / crosshair pick -> into the list (actors only)

    // ---- Save Actor Preset (replaces "skee preset-save <name>"); a_ref = the ticked Nearby Actor ----
    void SavePreset(RE::FormID a_ref, std::string a_name);
}
