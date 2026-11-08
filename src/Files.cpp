// Files - reads / writes the old Lazy Followers files (JContainers JSON) in the same folders and layout,
// and lists RaceMenu presets. Plain file I/O, safe on any thread; form lookups need the data loaded.
//
// Layout notes (from _LazyFollowersConfigManager.psc):
// - Lists are maps "0".."n". The old script wrote them LAST entry first (key 0 = last list entry) and read
//   them back reversed; we keep that order so old and new files stay interchangeable. (Its flipFormArray
//   was broken - loading voices / races from the config never worked in the old mod.)
// - Combat styles: "CombatStyle" -> "<n>" (n = 9 - style index) -> "<m>" (m = 23 - value index), floats.
// - NPC files: "NPC" (single) or "NPC0".."NPCn" (all, with "slotId") -> Actor, PresetName, Preset, IsFemale,
//   CombatStyle, VoiceType, Race, Class, Spells {..}, Equipment {..}. We add "HeadMesh".

#include "Files.h"

#include <charconv>

namespace Files
{
    namespace
    {
        // Lower-case copy (for case-insensitive compares).
        std::string Lower(std::string_view a_s)
        {
            std::string out(a_s);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        // Files in a_dir with extension a_ext (lower case, with the dot), file names only, sorted by name.
        std::vector<std::string> ListFiles(const std::filesystem::path& a_dir, std::string_view a_ext)
        {
            std::vector<std::string> out;
            std::error_code          ec;
            for (std::filesystem::directory_iterator it(a_dir, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) {
                    continue;
                }
                const auto& p = it->path();
                if (Lower(p.extension().string()) == a_ext) {
                    out.push_back(p.filename().string());
                }
            }
            std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
            return out;
        }

        // A map "0".."n" written last entry first (see the header): returns the entries in list order.
        template <class T, class F>
        std::vector<T> ReadReversed(const nlohmann::json* a_map, F a_convert)
        {
            std::vector<T> out;
            if (!a_map || !a_map->is_object()) {
                return out;
            }
            std::size_t count = 0;
            while (a_map->contains(std::to_string(count))) {
                ++count;
            }
            out.resize(count);
            for (std::size_t k = 0; k < count; ++k) {
                out[count - 1 - k] = a_convert((*a_map)[std::to_string(k)]);
            }
            return out;
        }

        // A list as a map "0".."n", last entry first (the old script's writeXListToDisk).
        template <class T, class F>
        nlohmann::json WriteReversed(const std::vector<T>& a_list, F a_convert)
        {
            nlohmann::json map = nlohmann::json::object();
            const auto     n = a_list.size();
            for (std::size_t k = 0; k < n; ++k) {
                map[std::to_string(k)] = a_convert(a_list[n - 1 - k]);
            }
            return map;
        }

        // A map of forms in any order (Spells, Equipment) -> form IDs found in this game.
        std::vector<RE::FormID> ReadFormSet(const nlohmann::json* a_map)
        {
            std::vector<RE::FormID> out;
            if (!a_map || !a_map->is_object()) {
                return out;
            }
            for (auto& [k, v] : a_map->items()) {
                if (auto f = FromFormData(v)) {
                    out.push_back(f->GetFormID());
                } else {
                    logger::info("NPC file: {} = {} not found in this game, skipped", k, v.is_string() ? v.get<std::string>() : v.dump());
                }
            }
            return out;
        }

        nlohmann::json WriteFormSet(const std::vector<RE::FormID>& a_ids)
        {
            nlohmann::json map = nlohmann::json::object();
            int            n = 0;
            for (auto id : a_ids) {
                if (auto f = RE::TESForm::LookupByID(id); f && !FormData(f).empty()) {
                    map[std::to_string(n++)] = FormData(f);
                }
            }
            return map;
        }

        std::string StrOf(const nlohmann::json* a_v)
        {
            return a_v && a_v->is_string() ? a_v->get<std::string>() : std::string{};
        }

        RE::FormID IdOf(const nlohmann::json* a_v)
        {
            auto f = a_v ? FromFormData(*a_v) : nullptr;
            return f ? f->GetFormID() : 0;
        }
    }

    const std::filesystem::path& ConfigFile()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/LazyFollowers/LazyFollowersConfig.json";
        return p;
    }

    const std::filesystem::path& NPCFile()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPC.json";
        return p;
    }

    const std::filesystem::path& NPCFolder()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs";
        return p;
    }

    const std::filesystem::path& PresetFolder()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/CharGen/Exported";
        return p;
    }

    const std::filesystem::path& SettingsFile()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGSettings.json";
        return p;
    }

    std::vector<std::string> ListPresets() { return ListFiles(PresetFolder(), ".jslot"); }

    std::vector<std::string> ListNPCPresets() { return ListFiles(NPCFolder(), ".json"); }

    std::string HeadMeshFor(const std::string& a_presetFile)
    {
        if (a_presetFile.size() <= 6) {
            return {};
        }
        // the MCM: preset name minus ".jslot" + ".nif" in the same folder
        const auto     path = "data/SKSE/Plugins/CharGen/Exported/" + a_presetFile.substr(0, a_presetFile.size() - 6) + ".nif";
        std::error_code ec;
        return std::filesystem::exists(path, ec) ? path : std::string{};
    }

    // Counts the preset's head parts (formIdentifier "<plugin>|<local hex>") flagged male-only / female-only.
    int PresetSex(const std::string& a_presetFile)
    {
        auto j = ReadJson(PresetFolder() / a_presetFile);
        auto parts = j ? Find(*j, "headParts") : nullptr;
        auto dh = RE::TESDataHandler::GetSingleton();
        if (!parts || !parts->is_array() || !dh) {
            return -1;
        }
        int female = 0, male = 0;
        for (auto& p : *parts) {
            auto id = p.is_object() ? Find(p, "formIdentifier") : nullptr;
            if (!id || !id->is_string()) {
                continue;
            }
            const auto text = id->get<std::string>();
            const auto bar = text.rfind('|');
            if (bar == std::string::npos) {
                continue;
            }
            RE::FormID local = 0;
            const auto hex = std::string_view(text).substr(bar + 1);
            if (std::from_chars(hex.data(), hex.data() + hex.size(), local, 16).ec != std::errc{}) {
                continue;
            }
            auto hp = dh->LookupForm<RE::BGSHeadPart>(local & 0xFFFFFF, text.substr(0, bar));
            if (!hp) {
                continue;
            }
            const bool f = hp->flags.all(RE::BGSHeadPart::Flag::kFemale);
            const bool m = hp->flags.all(RE::BGSHeadPart::Flag::kMale);
            if (f && !m) {
                ++female;
            } else if (m && !f) {
                ++male;
            }
        }
        return female > male ? 1 : male > female ? 0 : -1;
    }

    std::string SafeFileName(std::string_view a_name)
    {
        std::string out;
        for (char c : a_name) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) {
                continue;
            }
            out.push_back(c);
        }
        // trim spaces and dots at the ends (Windows drops trailing dots / spaces)
        while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
            out.pop_back();
        }
        while (!out.empty() && out.front() == ' ') {
            out.erase(out.begin());
        }
        if (out.size() > 64) {
            out.resize(64);
        }
        static constexpr std::array<std::string_view, 6> kReserved{ "con", "prn", "aux", "nul", "com1", "lpt1" };
        if (std::find(kReserved.begin(), kReserved.end(), Lower(out)) != kReserved.end()) {
            out += '_';
        }
        return out;
    }

    std::string FormData(const RE::TESForm* a_form)
    {
        if (!a_form) {
            return {};
        }
        const auto id = a_form->GetFormID();
        auto       file = a_form->GetFile(0);
        if (!file || (id >> 24) == 0xFF) {
            return {};  // created in game: no stable plugin + ID
        }
        const RE::FormID local = file->IsLight() ? (id & 0xFFF) : (id & 0xFFFFFF);
        return std::format("__formData|{}|0x{:x}", file->GetFilename(), local);
    }

    RE::TESForm* FromFormData(const nlohmann::json& a_value)
    {
        if (!a_value.is_string()) {
            return nullptr;
        }
        const auto s = a_value.get<std::string>();
        constexpr std::string_view kPrefix = "__formData|";
        if (!s.starts_with(kPrefix)) {
            return nullptr;
        }
        const auto bar = s.find('|', kPrefix.size());
        if (bar == std::string::npos) {
            return nullptr;
        }
        const auto plugin = s.substr(kPrefix.size(), bar - kPrefix.size());
        RE::FormID id = 0;
        try {
            id = static_cast<RE::FormID>(std::stoul(s.substr(bar + 1), nullptr, 16));
        } catch (...) {
            return nullptr;
        }
        auto dh = RE::TESDataHandler::GetSingleton();
        if (plugin.empty()) {
            return RE::TESForm::LookupByID(id);
        }
        return dh ? dh->LookupForm(id, plugin) : nullptr;
    }

    std::optional<nlohmann::json> ReadJson(const std::filesystem::path& a_path)
    {
        std::ifstream in(a_path);
        if (!in) {
            return std::nullopt;
        }
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            logger::warn("{} is not valid JSON", a_path.string());
            return std::nullopt;
        }
        return j;
    }

    bool WriteJson(const std::filesystem::path& a_path, const nlohmann::json& a_j)
    {
        std::error_code ec;
        std::filesystem::create_directories(a_path.parent_path(), ec);
        std::ofstream out(a_path, std::ios::trunc);
        if (!out) {
            logger::error("Could not write {}", a_path.string());
            return false;
        }
        out << a_j.dump(2);
        return static_cast<bool>(out);
    }

    const nlohmann::json* Find(const nlohmann::json& a_obj, std::string_view a_key)
    {
        if (!a_obj.is_object()) {
            return nullptr;
        }
        if (auto it = a_obj.find(std::string(a_key)); it != a_obj.end()) {
            return &*it;
        }
        const auto key = Lower(a_key);
        for (auto it = a_obj.begin(); it != a_obj.end(); ++it) {
            if (Lower(it.key()) == key) {
                return &*it;
            }
        }
        return nullptr;
    }

    nlohmann::json NPCToJson(const NPCData& a_npc, bool a_withSlot)
    {
        auto form = [](RE::FormID a_id) -> nlohmann::json {
            const auto s = FormData(RE::TESForm::LookupByID(a_id));
            return s.empty() ? nlohmann::json() : nlohmann::json(s);
        };
        nlohmann::json j = nlohmann::json::object();
        if (a_withSlot) {
            j["slotId"] = a_npc.slotId;
        }
        j["Actor"] = form(a_npc.actor);
        j["PresetName"] = a_npc.presetName;
        j["Preset"] = a_npc.preset;
        j["HeadMesh"] = a_npc.headMesh;
        j["IsFemale"] = a_npc.female;
        j["CombatStyle"] = form(a_npc.style);
        j["VoiceType"] = form(a_npc.voice);
        j["Race"] = form(a_npc.race);
        j["Class"] = form(a_npc.npcClass);
        j["Spells"] = WriteFormSet(a_npc.spells);
        j["Equipment"] = WriteFormSet(a_npc.equipment);
        return j;
    }

    std::optional<NPCData> NPCFromJson(const nlohmann::json& a_obj)
    {
        if (!a_obj.is_object()) {
            return std::nullopt;
        }
        NPCData n;
        n.actor = IdOf(Find(a_obj, "Actor"));
        if (auto v = Find(a_obj, "slotId"); v && v->is_number_integer()) {
            n.slotId = v->get<int>();
        }
        n.presetName = StrOf(Find(a_obj, "PresetName"));
        n.preset = StrOf(Find(a_obj, "Preset"));
        n.headMesh = StrOf(Find(a_obj, "HeadMesh"));
        if (auto v = Find(a_obj, "IsFemale"); v && v->is_number()) {
            n.female = v->get<int>();
        }
        n.style = IdOf(Find(a_obj, "CombatStyle"));
        n.voice = IdOf(Find(a_obj, "VoiceType"));
        n.race = IdOf(Find(a_obj, "Race"));
        n.npcClass = IdOf(Find(a_obj, "Class"));
        n.spells = ReadFormSet(Find(a_obj, "Spells"));
        n.equipment = ReadFormSet(Find(a_obj, "Equipment"));
        if (n.preset.empty()) {
            return std::nullopt;  // the scripts' npcDataToLoad: no preset = nothing to load
        }
        return n;
    }

    bool WriteConfig(const ConfigData& a_cfg)
    {
        auto formOf = [](const LF::Choice& c) -> nlohmann::json {
            const auto s = FormData(RE::TESForm::LookupByID(c.id));
            return s.empty() ? nlohmann::json() : nlohmann::json(s);
        };
        auto nameOf = [](const LF::Choice& c) { return nlohmann::json(c.name); };

        nlohmann::json j = nlohmann::json::object();
        j["FemaleVoice"] = WriteReversed(a_cfg.femaleVoices, formOf);
        j["FemaleVoiceNames"] = WriteReversed(a_cfg.femaleVoices, nameOf);
        j["MaleVoice"] = WriteReversed(a_cfg.maleVoices, formOf);
        j["MaleVoiceNames"] = WriteReversed(a_cfg.maleVoices, nameOf);
        j["Races"] = WriteReversed(a_cfg.races, formOf);
        j["RaceNames"] = WriteReversed(a_cfg.races, nameOf);

        nlohmann::json styles = nlohmann::json::object();
        nlohmann::json names = nlohmann::json::object();
        for (int i = 0; i < LF::kStyles; ++i) {
            const int      n = LF::kStyles - 1 - i;
            nlohmann::json values = nlohmann::json::object();
            for (int v = 0; v < LF::kStyleValues; ++v) {
                values[std::to_string(LF::kStyleValues - 1 - v)] = a_cfg.styles[i][v];
            }
            styles[std::to_string(n)] = values;
            names[std::to_string(n)] = a_cfg.styleNames[i];
        }
        j["CombatStyle"] = styles;
        j["CombatStyleNames"] = names;
        return WriteJson(ConfigFile(), j);
    }

    std::optional<ConfigData> ReadConfig()
    {
        auto j = ReadJson(ConfigFile());
        if (!j) {
            return std::nullopt;
        }
        ConfigData cfg;
        auto       readChoices = [&](const char* a_forms, const char* a_names) {
            auto ids = ReadReversed<RE::FormID>(Find(*j, a_forms), [](const nlohmann::json& v) {
                auto f = FromFormData(v);
                return f ? f->GetFormID() : RE::FormID(0);
            });
            auto names = ReadReversed<std::string>(Find(*j, a_names), [](const nlohmann::json& v) {
                return v.is_string() ? v.get<std::string>() : std::string{};
            });
            std::vector<LF::Choice> out;
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] != 0) {
                    out.push_back({ ids[i], i < names.size() ? names[i] : std::string{} });
                }
            }
            return out;
        };
        cfg.femaleVoices = readChoices("FemaleVoice", "FemaleVoiceNames");
        cfg.maleVoices = readChoices("MaleVoice", "MaleVoiceNames");
        cfg.races = readChoices("Races", "RaceNames");

        if (auto styles = Find(*j, "CombatStyle"); styles && styles->is_object()) {
            for (int i = 0; i < LF::kStyles; ++i) {
                auto s = Find(*styles, std::to_string(LF::kStyles - 1 - i));
                if (!s || !s->is_object()) {
                    continue;
                }
                bool ok = true;
                for (int v = 0; v < LF::kStyleValues; ++v) {
                    auto val = Find(*s, std::to_string(LF::kStyleValues - 1 - v));
                    if (!val || !val->is_number()) {
                        ok = false;
                        break;
                    }
                    cfg.styles[i][v] = val->get<float>();
                }
                cfg.hasStyle[i] = ok;
            }
        }
        if (auto names = Find(*j, "CombatStyleNames"); names && names->is_object()) {
            for (int i = 0; i < LF::kStyles; ++i) {
                if (auto n = Find(*names, std::to_string(LF::kStyles - 1 - i)); n && n->is_string()) {
                    cfg.styleNames[i] = n->get<std::string>();
                }
            }
        }
        return cfg;
    }
}
