#pragma once

// Files - the old mod's files, read and written in the same places and layout (JContainers JSON), so
// existing config / NPC files keep working and MO2 still puts new ones in Overwrite:
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersConfig.json   voices, races, combat styles
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPC.json      all followers ("NPC0".."NPCn")
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs/<name>.json   one follower ("NPC")
//   Data/SKSE/Plugins/CharGen/Exported/<name>.jslot (+ .nif)   RaceMenu presets (read; written by RaceMenu)
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGSettings.json   Settings page (new: same in every save)
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGKeep.json   Keep in New Games (new: the followers at the last save)

#include "LF.h"

namespace Files
{
    const std::filesystem::path& ConfigFile();
    const std::filesystem::path& NPCFile();
    const std::filesystem::path& NPCFolder();
    const std::filesystem::path& PresetFolder();
    const std::filesystem::path& SettingsFile();
    const std::filesystem::path& KeepFile();  // NG (user): followers for Keep in New Games

    std::vector<std::string> ListPresets();     // "<name>.jslot", sorted
    bool                     HidePreset(const std::string& a_presetFile);  // NG (user): .jslot + its .dds / .nif -> "<file>.hidden"
    std::vector<std::string> ListHiddenPresets();  // "<name>.jslot.hidden" (or ".jslot.<n>.hidden"), sorted
    bool                     UnhidePreset(const std::string& a_hiddenFile, std::string& a_error);  // NG (user): back to .jslot (+ .dds / .nif)
    bool                     DeletePreset(const std::string& a_file, std::string& a_error);  // NG (user): .jslot (or hidden) + its .dds / .nif deleted
    std::vector<std::string> ListNPCPresets();  // "<name>.json", sorted
    std::vector<std::string> ListErinPresets(); // NG (user): Data/SKSE/Plugins/ErinRace/Preset/*.ini, sorted
    std::string              ErinResetPreset(const std::string& a_oldPreset);  // NG: writes the neutral Erin reset preset, returns its name for Erin ("" = failed)
    // NG (user): the Mu Dynamic NPC Replacer config (Data/SKSE/Plugins/MuDynamicNPCReplacer/*.json) that targets this
    // base NPC (IsActorBase(<plugin>|0x<local id>)) - its file name, "" = none
    std::string MuReplacerConfig(std::string_view a_plugin, std::uint32_t a_localId);
    std::string              HeadMeshFor(const std::string& a_presetFile);  // "" or the .nif path the scripts used
    // The sex a RaceMenu preset was made for (.jslot has no sex field): its head parts' Male / Female flags.
    // 1 = female, 0 = male, -1 = unknown (no file, or no sex-only head part). Main thread (form lookups).
    int PresetSex(const std::string& a_presetFile);
    // NG (user): the race a preset was made for, from its head parts' valid-race lists. 0 = unknown. Main thread.
    RE::FormID PresetRace(const std::string& a_presetFile);
    // NG: an empty RaceMenu preset (written once); its key for CharGen.LoadCharacterEx, "" = could not write it.
    // Loading it erases RaceMenu's per-NPC sculpt data and custom face morphs (Remove on a real NPC).
    std::string CleanPresetKey();
    // NG 1.2: the preset's hair colour (.jslot "actor" / "hairColor", 0xRRGGBB). nullopt = no file / no value.
    std::optional<std::uint32_t> PresetHairColor(const std::string& a_presetFile);

    // A name the user typed, made safe as a file name: no path or Windows-reserved characters, trimmed,
    // at most 64 characters. "" = nothing usable left.
    std::string SafeFileName(std::string_view a_name);

    // JContainers' form format: "__formData|<plugin>|0x<local id>".
    std::string    FormData(const RE::TESForm* a_form);
    RE::TESForm*   FromFormData(const nlohmann::json& a_value);

    std::optional<nlohmann::json> ReadJson(const std::filesystem::path& a_path);
    bool                          WriteJson(const std::filesystem::path& a_path, const nlohmann::json& a_j);

    // Case-insensitive key lookup (JContainers keys are case-insensitive; the old scripts wrote "IsFemale"
    // but read "isFemale").
    const nlohmann::json* Find(const nlohmann::json& a_obj, std::string_view a_key);

    // One follower as stored in the NPC files.
    struct NPCData
    {
        RE::FormID              actor = 0;
        int                     slotId = -1;  // LazyFollowersNPC.json only
        std::string             presetName;
        std::string             preset;
        std::string             headMesh;     // new key (old files have none)
        std::string             erinPreset;   // NG key "ErinPreset": Erin race extra preset "<file>.ini" ("" = none)
        int                     female = -1;  // -1 = not in the file
        RE::FormID              style = 0;
        RE::FormID              voice = 0;
        RE::FormID              race = 0;
        RE::FormID              npcClass = 0;
        std::vector<RE::FormID> spells;
        std::vector<RE::FormID> equipment;
    };
    nlohmann::json         NPCToJson(const NPCData& a_npc, bool a_withSlot);
    std::optional<NPCData> NPCFromJson(const nlohmann::json& a_obj);

    // LazyFollowersConfig.json.
    struct ConfigData
    {
        std::vector<LF::Choice> femaleVoices;
        std::vector<LF::Choice> maleVoices;
        std::vector<LF::Choice> races;
        std::array<std::array<float, LF::kStyleValues>, LF::kStyles> styles{};
        std::array<bool, LF::kStyles>                                hasStyle{};
        std::array<std::string, LF::kStyles>                         styleNames;
    };
    bool                      WriteConfig(const ConfigData& a_cfg);
    std::optional<ConfigData> ReadConfig();
}
