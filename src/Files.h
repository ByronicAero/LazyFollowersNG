#pragma once

// Files - the old mod's files, read and written in the same places and layout (JContainers JSON), so
// existing config / NPC files keep working and MO2 still puts new ones in Overwrite:
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersConfig.json   voices, races, combat styles
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPC.json      all followers ("NPC0".."NPCn")
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs/<name>.json   one follower ("NPC")
//   Data/SKSE/Plugins/CharGen/Exported/<name>.jslot (+ .nif)   RaceMenu presets (read; written by RaceMenu)
//   Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGSettings.json   Settings page (new: same in every save)

#include "LF.h"

namespace Files
{
    const std::filesystem::path& ConfigFile();
    const std::filesystem::path& NPCFile();
    const std::filesystem::path& NPCFolder();
    const std::filesystem::path& PresetFolder();
    const std::filesystem::path& SettingsFile();

    std::vector<std::string> ListPresets();     // "<name>.jslot", sorted
    std::vector<std::string> ListNPCPresets();  // "<name>.json", sorted
    std::string              HeadMeshFor(const std::string& a_presetFile);  // "" or the .nif path the scripts used
    // The sex a RaceMenu preset was made for (.jslot has no sex field): its head parts' Male / Female flags.
    // 1 = female, 0 = male, -1 = unknown (no file, or no sex-only head part). Main thread (form lookups).
    int PresetSex(const std::string& a_presetFile);

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
