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
// - Our esp is ESL-flagged with renumbered IDs: "LazyFollowers.esp" forms in files written with the old esp
//   (IDs above 0xFFF) are translated with EslIds.h when read.

#include "Files.h"

#include "EslIds.h"

#include <charconv>
#include <set>
#include <sstream>

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

    // NG (user): Keep in New Games - the followers written at every game save, in LazyFollowersNPC.json's layout.
    const std::filesystem::path& KeepFile()
    {
        static const std::filesystem::path p = "Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGKeep.json";
        return p;
    }

    std::vector<std::string> ListPresets() { return ListFiles(PresetFolder(), ".jslot"); }

    // NG (user): the Erin race's extra presets (its own .ini: extra tints, eye colour set, skin, head-part adjust).
    std::vector<std::string> ListErinPresets() { return ListFiles("Data/SKSE/Plugins/ErinRace/Preset", ".ini"); }

    // The head-part slots an Erin preset moves: the numbers before ':' in its "HeadPartAdjust = <slot>:<11 values>, ..."
    // line (format of Erin's own saved presets).
    static void ErinAdjustSlots(const std::filesystem::path& a_file, std::set<int>& a_slots)
    {
        std::ifstream in(a_file);
        std::string   line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos || line.compare(0, 14, "HeadPartAdjust") != 0) {
                continue;
            }
            std::stringstream entries(line.substr(eq + 1));
            std::string       entry;
            while (std::getline(entries, entry, ',')) {
                const auto colon = entry.find(':');
                if (colon == std::string::npos) {
                    continue;
                }
                try {
                    a_slots.insert(std::stoi(entry.substr(0, colon)));
                } catch (...) {
                }
            }
        }
    }

    // NG fix (Erin limit): an Erin preset that only sets back what Erin keeps after the preset is off - every head-part
    // slot a_oldPreset moved, neutral (position / rotation 0, scale 1, pivot 0, no mirror), and TriMorphType Default.
    // Old preset gone: the slots of every preset in ErinRace/Preset. Written to Data/SKSE/Plugins/LazyFollowersNG/
    // ErinReset.ini (MO2: Overwrite); returns the name Erin takes (relative to Data/SKSE/Plugins), "" when no slot.
    std::string ErinResetPreset(const std::string& a_oldPreset)
    {
        const std::filesystem::path folder = "Data/SKSE/Plugins/ErinRace/Preset";
        std::set<int>               slots;
        std::error_code             ec;
        if (!a_oldPreset.empty() && std::filesystem::exists(folder / a_oldPreset, ec)) {
            ErinAdjustSlots(folder / a_oldPreset, slots);
        } else {
            for (const auto& f : ListErinPresets()) {
                ErinAdjustSlots(folder / f, slots);
            }
        }
        if (slots.empty()) {
            return {};
        }
        const std::filesystem::path out = "Data/SKSE/Plugins/LazyFollowersNG";
        std::filesystem::create_directories(out, ec);
        std::ofstream file(out / "ErinReset.ini", std::ios::trunc);
        if (!file) {
            return {};
        }
        file << "HeadPartAdjust = ";
        bool first = true;
        for (int slot : slots) {
            file << (first ? "" : ", ") << slot << ":0.000000|0.000000|0.000000|0.000000|0.000000|0.000000|1.000000|1.000000|1.000000|0|False";
            first = false;
        }
        file << "\n\nTriMorphType = Default\n";
        file.close();
        return file ? "LazyFollowersNG\\ErinReset.ini" : std::string{};
    }

    // NG (user crash 2026-11-12: Mu Dynamic NPC Replacer put its preset on Sofia while she was our Override): its MCM's
    // exported configs (<Plugin>_0x<id>.json, MO2 puts them in Overwrite) name the NPC as IsActorBase(<plugin>|0x<id>).
    // Every .json in its folder is read (lower-case) and each IsActorBase(<plugin>| is compared by number, so "0x12c4" and
    // "0x000012C4" both match.
    std::string MuReplacerConfig(std::string_view a_plugin, std::uint32_t a_localId)
    {
        const std::filesystem::path dir = "Data/SKSE/Plugins/MuDynamicNPCReplacer";
        const std::string           key = "isactorbase(" + Lower(a_plugin) + "|";
        for (const auto& file : ListFiles(dir, ".json")) {
            std::ifstream in(dir / file, std::ios::binary);
            if (!in) {
                continue;
            }
            const std::string text = Lower(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            for (auto pos = text.find(key); pos != std::string::npos; pos = text.find(key, pos + 1)) {
                auto p = pos + key.size();
                while (p < text.size() && text[p] == ' ') {
                    ++p;
                }
                if (text.compare(p, 2, "0x") == 0) {
                    p += 2;
                }
                std::uint32_t id = 0;
                const auto    end = text.data() + text.size();
                if (auto [ptr, ec] = std::from_chars(text.data() + p, end, id, 16); ec == std::errc{} && id == a_localId) {
                    return file;
                }
            }
        }
        return {};
    }

    namespace
    {
        // Renames a_file to "<a_file>.hidden" ("<a_file>.2.hidden"... when that exists). false = not there / failed.
        bool HideFile(const std::filesystem::path& a_file)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(a_file, ec)) {
                return false;
            }
            auto target = std::filesystem::path(a_file.string() + ".hidden");
            for (int n = 2; std::filesystem::exists(target, ec) && n < 100; ++n) {
                target = std::filesystem::path(std::format("{}.{}.hidden", a_file.string(), n));
            }
            std::filesystem::rename(a_file, target, ec);
            if (ec) {
                logger::warn("Could not hide {}: {}", a_file.string(), ec.message());
                return false;
            }
            logger::info("Hid {} -> {}", a_file.string(), target.filename().string());
            return true;
        }
    }

    // NG (user, Settings > Presets): hides one preset instead of deleting it - "<name>.jslot", its tint
    // Textures/CharGen/Exported/<name>.dds and head mesh <name>.nif get ".hidden" added (rename back to undo).
    // Only a plain "<name>.jslot" in the preset folder (no folders in the name). False = the .jslot was not hidden.
    bool HidePreset(const std::string& a_presetFile)
    {
        const std::filesystem::path name(a_presetFile);
        if (a_presetFile.empty() || name.has_parent_path() || Lower(name.extension().string()) != ".jslot") {
            return false;
        }
        if (!HideFile(PresetFolder() / name)) {
            return false;
        }
        const auto stem = name.stem().string();
        HideFile(std::filesystem::path("Data/Textures/CharGen/Exported") / (stem + ".dds"));
        HideFile(PresetFolder() / (stem + ".nif"));
        return true;
    }

    // Hidden presets: "<name>.jslot.hidden" / "<name>.jslot.<n>.hidden" in the preset folder, sorted.
    std::vector<std::string> ListHiddenPresets()
    {
        std::vector<std::string> out;
        for (auto& f : ListFiles(PresetFolder(), ".hidden")) {
            if (Lower(f).find(".jslot.") != std::string::npos) {
                out.push_back(f);
            }
        }
        return out;
    }

    // NG (user): undoes HidePreset - "<name>.jslot<suffix>" back to "<name>.jslot", and the .dds / .nif with
    // the same suffix (else plain ".hidden"). Stops when "<name>.jslot" exists again.
    bool UnhidePreset(const std::string& a_hiddenFile, std::string& a_error)
    {
        const auto lower = Lower(a_hiddenFile);
        const auto pos = lower.find(".jslot.");
        if (pos == std::string::npos || std::filesystem::path(a_hiddenFile).has_parent_path() || !lower.ends_with(".hidden")) {
            a_error = "not a hidden preset";
            return false;
        }
        const auto      stem = a_hiddenFile.substr(0, pos);
        const auto      suffix = a_hiddenFile.substr(pos + 6);  // ".hidden" or ".<n>.hidden"
        const auto      jslot = PresetFolder() / (stem + ".jslot");
        std::error_code ec;
        if (std::filesystem::exists(jslot, ec)) {
            a_error = stem + ".jslot already exists";
            return false;
        }
        std::filesystem::rename(PresetFolder() / a_hiddenFile, jslot, ec);
        if (ec) {
            a_error = ec.message();
            logger::warn("Could not unhide {}: {}", a_hiddenFile, a_error);
            return false;
        }
        logger::info("Unhid {}", a_hiddenFile);
        // the tint and head mesh, if they were hidden with it
        for (const auto& plain : { std::filesystem::path("Data/Textures/CharGen/Exported") / (stem + ".dds"), PresetFolder() / (stem + ".nif") }) {
            if (std::filesystem::exists(plain, ec)) {
                continue;
            }
            for (const auto& hidden : { std::filesystem::path(plain.string() + suffix), std::filesystem::path(plain.string() + ".hidden") }) {
                if (std::filesystem::is_regular_file(hidden, ec)) {
                    std::filesystem::rename(hidden, plain, ec);
                    if (!ec) {
                        logger::info("Unhid {}", hidden.string());
                    }
                    break;
                }
            }
        }
        return true;
    }

    // NG (user: "Hide, Unhide, Delete files - multiple"): deletes one preset for good - "<name>.jslot" with its tint
    // Textures/CharGen/Exported/<name>.dds and head mesh <name>.nif, or a hidden "<name>.jslot<suffix>" with the
    // .dds / .nif carrying the same suffix (else plain ".hidden"). A hidden preset never touches the plain files.
    // Under MO2 the file is deleted from the mod (or Overwrite) it is in. False = the preset file itself stayed.
    bool DeletePreset(const std::string& a_file, std::string& a_error)
    {
        const auto lower = Lower(a_file);
        const auto pos = lower.find(".jslot");
        if (a_file.empty() || std::filesystem::path(a_file).has_parent_path() || pos == std::string::npos ||
            !(lower.ends_with(".jslot") || lower.ends_with(".hidden"))) {
            a_error = "not a preset file";
            return false;
        }
        const auto      stem = a_file.substr(0, pos);
        const auto      suffix = a_file.substr(pos + 6);  // "" or ".hidden" / ".<n>.hidden"
        std::error_code ec;
        if (!std::filesystem::remove(PresetFolder() / a_file, ec) || ec) {
            a_error = ec ? ec.message() : "file not found";
            logger::warn("Could not delete {}: {}", a_file, a_error);
            return false;
        }
        logger::info("Deleted {}", a_file);
        for (const auto& plain : { std::filesystem::path("Data/Textures/CharGen/Exported") / (stem + ".dds"), PresetFolder() / (stem + ".nif") }) {
            std::vector<std::filesystem::path> tries{ std::filesystem::path(plain.string() + suffix) };
            if (!suffix.empty() && suffix != ".hidden") {
                tries.emplace_back(plain.string() + ".hidden");
            }
            for (const auto& f : tries) {
                if (std::filesystem::is_regular_file(f, ec) && std::filesystem::remove(f, ec)) {
                    logger::info("Deleted {}", f.string());
                    break;
                }
            }
        }
        return true;
    }

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

    // NG fix (user video 2026-11-12 09-45-28: Sofia's face still mashed with Ciel's after Remove): RaceMenu keeps a
    // preset's sculpt data and custom face morphs per base NPC and puts them on every head it builds; ClearPreset and
    // no Papyrus function removes them - only a new preset apply does (PresetInterface::ApplyPreset: EraseSculptData,
    // EraseMorphData, then the preset's). This writes an empty preset (no head parts, morphs, sculpt, tints) for that.
    // Data/SKSE/Plugins/CharGen/LazyFollowersNG/Clean.jslot - not in Exported, so it is not in the Preset list.
    std::string CleanPresetKey()
    {
        static const std::filesystem::path file = "Data/SKSE/Plugins/CharGen/LazyFollowersNG/Clean.jslot";
        std::error_code                    ec;
        if (!std::filesystem::exists(file, ec)) {
            nlohmann::json j = { { "version", { { "signature", 1163086675u }, { "formatVersion", 3 } } },  // 'SKSE', as RaceMenu writes it
                { "modNames", nlohmann::json::array({ "Skyrim.esm" }) },
                { "headParts", nlohmann::json::array() },
                { "morphs", { { "custom", nlohmann::json::array() }, { "sculpt", nlohmann::json::array() } } } };
            if (!WriteJson(file, j)) {
                return {};
            }
        }
        return "../LazyFollowersNG/Clean";  // CharGen.LoadCharacterEx adds SKSE\Plugins\CharGen\Exported\ + .jslot
    }

    // The preset's "actor" / "hairColor". NG cleanup: cached per file and its write time (it was parsed from the
    // whole .jslot - sculpt data can make it large - on every apply and load of every follower).
    std::optional<std::uint32_t> PresetHairColor(const std::string& a_presetFile)
    {
        struct Cached
        {
            std::filesystem::file_time_type time;
            std::optional<std::uint32_t>    color;
        };
        static std::mutex                              s_lock;
        static std::unordered_map<std::string, Cached> s_cache;

        const auto      path = PresetFolder() / a_presetFile;
        std::error_code ec;
        const auto      time = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return std::nullopt;  // no file
        }
        const auto key = Lower(a_presetFile);
        {
            std::lock_guard lock(s_lock);
            if (auto it = s_cache.find(key); it != s_cache.end() && it->second.time == time) {
                return it->second.color;
            }
        }
        std::optional<std::uint32_t> color;
        auto j = ReadJson(path);
        auto actor = j ? Find(*j, "actor") : nullptr;
        auto c = actor && actor->is_object() ? Find(*actor, "hairColor") : nullptr;
        if (c && c->is_number_integer()) {
            color = static_cast<std::uint32_t>(c->get<std::int64_t>() & 0xFFFFFF);
        }
        std::lock_guard lock(s_lock);
        s_cache[key] = { time, color };
        return color;
    }

    namespace
    {
        // The head parts of a preset's "headParts" (formIdentifier "<plugin>|<local hex>") found in this game.
        std::vector<RE::BGSHeadPart*> PresetHeadParts(const nlohmann::json& a_preset)
        {
            std::vector<RE::BGSHeadPart*> out;
            auto parts = Find(a_preset, "headParts");
            auto dh = RE::TESDataHandler::GetSingleton();
            if (!parts || !parts->is_array() || !dh) {
                return out;
            }
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
                if (auto hp = dh->LookupForm<RE::BGSHeadPart>(local & 0xFFFFFF, text.substr(0, bar))) {
                    out.push_back(hp);
                }
            }
            return out;
        }
    }

    // NG (user): the race a preset was made for. A .jslot has no race field, so every head part votes for the races
    // of its valid-race list (RNAM; the face part counts 3, the rest 1); the race with most votes wins, ties go to a
    // playable, then a non-vampire race. Cached per file + write time (main thread: form lookups). 0 = unknown.
    RE::FormID PresetRace(const std::string& a_presetFile)
    {
        struct Cached
        {
            std::filesystem::file_time_type time;
            RE::FormID                      race = 0;
        };
        static std::unordered_map<std::string, Cached> s_cache;

        const auto      path = PresetFolder() / a_presetFile;
        std::error_code ec;
        const auto      time = std::filesystem::last_write_time(path, ec);
        if (ec) {
            return 0;
        }
        const auto key = Lower(a_presetFile);
        if (auto it = s_cache.find(key); it != s_cache.end() && it->second.time == time) {
            return it->second.race;
        }
        std::unordered_map<RE::TESRace*, int> votes;
        if (auto j = ReadJson(path)) {
            for (auto hp : PresetHeadParts(*j)) {
                if (!hp->validRaces) {
                    continue;
                }
                const int weight = hp->type == RE::BGSHeadPart::HeadPartType::kFace ? 3 : 1;
                for (auto f : hp->validRaces->forms) {
                    if (auto r = f ? f->As<RE::TESRace>() : nullptr) {
                        votes[r] += weight;
                    }
                }
            }
        }
        auto rank = [](RE::TESRace* a_r, int a_votes) {
            const bool playable = a_r->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
            const bool vampire = a_r->HasKeywordString("Vampire");
            return std::tuple{ a_votes, playable, !vampire, -static_cast<std::int64_t>(a_r->GetFormID()) };
        };
        RE::TESRace* best = nullptr;
        int          bestVotes = 0;
        for (auto& [r, n] : votes) {
            if (!best || rank(r, n) > rank(best, bestVotes)) {
                best = r;
                bestVotes = n;
            }
        }
        // NG fix (user: Vicky.jslot made as Nord UBE was shown as "Breton UBE (preset's race)"): UBE's head parts (and tint
        // masks) are the same for all its races, so several races tie - the lowest form ID won. A tie between races of the
        // same kind (playable, vampire or not) = unknown: no race picked or marked for the preset.
        int tied = 0;
        for (auto& [r, n] : votes) {
            if (best && r != best && n == bestVotes && std::get<1>(rank(r, n)) == std::get<1>(rank(best, bestVotes)) &&
                std::get<2>(rank(r, n)) == std::get<2>(rank(best, bestVotes))) {
                ++tied;
            }
        }
        if (best && tied > 0) {
            logger::info("Preset {}: race unknown - its head parts fit {} races equally (e.g. {})", a_presetFile, tied + 1, best->GetFullName());
            s_cache[key] = { time, 0 };
            return 0;
        }
        const RE::FormID race = best ? best->GetFormID() : 0;
        if (best) {
            const char* ed = best->GetFormEditorID();
            logger::info("Preset {}: made for race {} {:08X} ({} votes of its head parts)", a_presetFile, ed && *ed ? ed : best->GetFullName(), race, bestVotes);
        } else {
            logger::info("Preset {}: race not found (no head part with a valid-race list)", a_presetFile);
        }
        s_cache[key] = { time, race };
        return race;
    }

    // Counts the preset's head parts (formIdentifier "<plugin>|<local hex>") flagged male-only / female-only.
    int PresetSex(const std::string& a_presetFile)
    {
        auto j = ReadJson(PresetFolder() / a_presetFile);
        if (!j) {
            return -1;
        }
        int female = 0, male = 0;
        for (auto hp : PresetHeadParts(*j)) {
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
        if (id > 0xFFF && Lower(plugin) == "lazyfollowers.esp") {
            id = LF::EslId(id);  // file written with the old (non-ESL) esp: its IDs were renumbered
            if (!id) {
                return nullptr;
            }
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
        if (!a_npc.erinPreset.empty()) {
            j["ErinPreset"] = a_npc.erinPreset;  // NG key (the old mod's files have none)
        }
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
        n.erinPreset = StrOf(Find(a_obj, "ErinPreset"));
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
