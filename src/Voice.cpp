// Voice - modded voices on Lazy Followers followers (NG 1.2, user: Sofia's voice pack, video "Custom Voice not
// the same voice"). A custom voice type only has audio for its own mod's lines (Sound\Voice\<mod>\<voice>\...),
// so the vanilla / RDO follower lines a Lazy Followers follower says with it are silent.
// The game builds every spoken line in DialogueItem::Ctor (ID 34413 / 35220: the responses = text + voice file
// of the info for the speaker's voice type). Every direct call of it in the game is hooked. For a Lazy
// Followers follower (base NPC from LazyFollowers.esp) with a voice type that is not from the base game, the
// info is pointed (shared-line link TESTopicInfo::dataInfo, see RedirectSaid / PatchMenu) at a matching line of
// the voice's own mod, so the game takes that line's text + voice file: Hello / Goodbye -> its
// Hello / Goodbye lines; Follow / Wait / Trade / Dismiss / Favor (vanilla DialogueFollower...Topic) -> its
// custom topic whose Editor ID ends in FollowMeTopic / FollowTopic, WaitTopic, TradeTopic, PartWaysTopic /
// DismissTopic, DoFavourTopic.... The item keeps its own info / topic / quest, so the original line's
// scripts run (Lazy Followers' follow / wait), never the voice mod's (they act on that mod's own NPC).
// Lines of the voice mod = infos with a condition GetIsVoiceType <voice> or GetIsID <an NPC with that voice>.
// Their own-mod conditions (GetIsID / GetInFaction / GetIsVoiceType on that mod's forms) are answered as if the
// follower were that mod's NPC (faction = follower is your teammate, see OwnModCondition), the
// rest are checked on the follower. No match: the line stays as it was.
// Talk menu answers (Follow / Wait / Trade...) are built on click by another path: while the talk menu is open,
// the listed answers take their text + voice from the picked line via the game's shared-line link (PatchMenu).
// NG 1.2 (user): a real NPC follower with a nickname (LF::Nickname): its original name in the text of its lines
// (subtitles) and of its talk menu entries is replaced by the nickname. The recorded audio stays as it is.

#include "Voice.h"

#include "LF.h"

#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <shared_mutex>
#include <unordered_set>

#include <zlib.h>

namespace Voice
{
    namespace
    {
        using Subtype = RE::DIALOGUE_DATA::Subtype;
        using Func = RE::FUNCTION_DATA::FunctionID;
        using Object = RE::CONDITIONITEMOBJECT;

        // One line (info) of a voice's mod and its topic.
        struct Line
        {
            RE::TESTopic*     topic;
            RE::TESTopicInfo* info;
        };

        std::mutex                                        g_lock;
        std::unordered_map<RE::FormID, std::vector<Line>> g_lines;  // voice -> its mod's lines (Prepare)
        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_owners;  // NG 1.2: voice -> NPCs its plugin gives it (Prepare)
        std::uintptr_t                                    g_ctor = 0;  // the game's DialogueItem::Ctor

        // NG 1.2 (user: "it should say whatever the user nickname is"): a_from as a whole word in a_text -> a_to.
        // Not done when the nickname itself contains the original name (it would grow on every pass).
        bool ReplaceName(std::string& a_text, const std::string& a_from, const std::string& a_to)
        {
            auto word = [](const std::string& a_s, std::size_t a_pos, std::size_t a_len) {
                const bool start = a_pos == 0 || !std::isalnum(static_cast<unsigned char>(a_s[a_pos - 1]));
                const bool end = a_pos + a_len >= a_s.size() || !std::isalnum(static_cast<unsigned char>(a_s[a_pos + a_len]));
                return start && end;
            };
            for (auto p = a_to.find(a_from); p != std::string::npos; p = a_to.find(a_from, p + 1)) {
                if (word(a_to, p, a_from.size())) {
                    return false;
                }
            }
            bool changed = false;
            for (auto p = a_text.find(a_from); p != std::string::npos; p = a_text.find(a_from, p)) {
                if (word(a_text, p, a_from.size())) {
                    a_text.replace(p, a_from.size(), a_to);
                    p += a_to.size();
                    changed = true;
                } else {
                    p += 1;
                }
            }
            return changed;
        }

        // The nickname in a BSString (a line's text / a talk menu entry).
        void RenameText(RE::BSString& a_text, const std::string& a_from, const std::string& a_to)
        {
            const char* t = a_text.c_str();
            if (!t || !*t) {
                return;
            }
            std::string s(t);
            if (ReplaceName(s, a_from, a_to)) {
                a_text = s.c_str();
            }
        }

        // The nickname in every response of a list (a built line, a talk menu entry's answer).
        void RenameResponses(RE::BSSimpleList<RE::DialogueResponse*>& a_list, const std::string& a_from, const std::string& a_to)
        {
            for (auto r : a_list) {
                if (r) {
                    RenameText(r->text, a_from, a_to);
                }
            }
        }

        // A talk menu entry of a real NPC follower with a nickname (the talk menu's speaker): the player's line
        // and the answer.
        void RenameMenuEntry(RE::MenuTopicManager::Dialogue* a_entry)
        {
            auto mtm = RE::MenuTopicManager::GetSingleton();
            auto ref = mtm ? mtm->speaker.get() : RE::NiPointer<RE::TESObjectREFR>{};
            if (!a_entry || !ref) {
                return;
            }
            std::string from, to;
            if (LF::Nickname(ref->GetFormID(), from, to)) {
                RenameText(a_entry->topicText, from, to);
                RenameResponses(a_entry->responses, from, to);
            }
        }

        // Follower topics swapped by Editor ID: the vanilla topic's ending -> the voice mod topic's endings (compared
        // with TopicStem: Sofia's JJSofiaDialogueWaitTopic and Severa's SR01FollowerSevWait01 both end in Wait).
        struct TopicKey
        {
            std::string_view                src;
            std::array<std::string_view, 4> dst;
        };
        constexpr TopicKey kKeys[] = {
            { "FollowerFollowTopic", { "FollowMe", "Follow" } },
            { "FavorGenericFollowBranchTopic", { "FollowMe", "Follow" } },  // "Follow me, I need your help" (recruit)
            { "FollowerWaitTopic", { "Wait" } },
            { "FollowerTradeTopic", { "Trade" } },
            { "FollowerDismissTopic", { "PartWays", "Dismiss" } },
            { "FollowerFavorStateTopic", { "DoFavour", "DoFavor", "Favour", "Favor" } },
        };

        bool EqualsI(std::string_view a_a, std::string_view a_b)
        {
            return a_a.size() == a_b.size() && std::equal(a_a.begin(), a_a.end(), a_b.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        }

        bool EndsWithI(std::string_view a_s, std::string_view a_end)
        {
            return a_s.size() >= a_end.size() && EqualsI(a_s.substr(a_s.size() - a_end.size()), a_end);
        }

        std::string_view EditorID(const RE::TESTopic* a_topic)
        {
            const char* e = a_topic ? a_topic->GetFormEditorID() : nullptr;
            return e ? e : "";
        }

        // A voice mod topic's Editor ID without a numbered ending ("A1", "01", user log: Severa's
        // SR01FollowerSevFollowA1) and without "Topic" (JJSofiaDialogueFollowMeTopic -> ...FollowMe).
        std::string_view TopicStem(std::string_view a_edid)
        {
            auto s = a_edid;
            while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.back()))) {
                s.remove_suffix(1);
            }
            if (s.size() < a_edid.size() && s.size() >= 2 && std::isupper(static_cast<unsigned char>(s.back())) &&
                std::islower(static_cast<unsigned char>(s[s.size() - 2]))) {
                s.remove_suffix(1);  // the letter of "A1"
            }
            if (EndsWithI(s, "Topic")) {
                s.remove_suffix(5);
            }
            return s;
        }

        // Skyrim.esm and the DLCs: their voices have audio for the vanilla lines (never swapped).
        bool BaseGameFile(const RE::TESFile* a_file)
        {
            if (!a_file) {
                return true;
            }
            static constexpr std::string_view kBase[] = { "Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm" };
            for (auto b : kBase) {
                if (EqualsI(a_file->GetFilename(), b)) {
                    return true;
                }
            }
            return false;
        }

        // The condition picks a_voice: GetIsVoiceType <voice> or GetIsID <an NPC that has that voice> - in the game
        // or, a_fileNpcs, in its plugin file (user log 2026-11-11: Severa's NPC SR01Sevvy has her voice
        // SR01VoiceUniqueSev in SR01.esp but not in the game, so only 34 of her ~380 lines were found).
        bool NamesVoice(const RE::TESConditionItem* a_c, const RE::BGSVoiceType* a_voice, const std::unordered_set<RE::FormID>* a_fileNpcs = nullptr)
        {
            const auto fn = a_c->data.functionData.function.get();
            auto       form = static_cast<RE::TESForm*>(a_c->data.functionData.params[0]);
            if (!form) {
                return false;
            }
            if (fn == Func::kGetIsVoiceType) {
                return form == a_voice;
            }
            if (fn == Func::kGetIsID) {
                auto npc = form->As<RE::TESNPC>();
                return npc && (npc->voiceType == a_voice || (a_fileNpcs && a_fileNpcs->contains(npc->GetFormID())));
            }
            return false;
        }

        // a_value compared like the condition item says (==, !=, >, >=, <, <=).
        bool Compare(float a_value, const RE::TESConditionItem* a_c)
        {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            const float v = a_c->data.flags.global ? (a_c->data.comparisonValue.g ? a_c->data.comparisonValue.g->value : 0.0f) :
                                                     a_c->data.comparisonValue.f;
            switch (a_c->data.flags.opCode) {
            case Op::kEqualTo:
                return a_value == v;
            case Op::kNotEqualTo:
                return a_value != v;
            case Op::kGreaterThan:
                return a_value > v;
            case Op::kGreaterThanOrEqualTo:
                return a_value >= v;
            case Op::kLessThan:
                return a_value < v;
            case Op::kLessThanOrEqualTo:
                return a_value <= v;
            default:
                return false;
            }
        }

        // A condition on the voice mod's own NPC / faction / voice (form from the line's own plugin, run on the
        // speaker): answered for our follower as if it were that NPC (NG, user: "Why should I follow you?" when
        // already following). GetIsID -> 1 (it is "that NPC"); GetInFaction -> 1 while the follower is your
        // teammate (the mod's follower faction); GetIsVoiceType -> the follower's voice. Returns false when the
        // condition is not one of these (checked by the game then).
        // a_loose (2nd try when no line passed, user log: Sofia's Trade / Follow me lines need her global
        // JJSofiaHasMetPlayer): every GetGlobalValue condition counts as true.
        bool OwnModCondition(const RE::TESConditionItem* a_c, const RE::TESFile* a_mod, RE::Actor* a_speaker, bool a_loose, bool& a_result)
        {
            const auto fn = a_c->data.functionData.function.get();
            if (fn == Func::kIsGreetingPlayer) {
                // checked while the greeting is being built, so it is always "greeting" then (user log: Sofia's
                // hellos need IsGreetingPlayer == 0 and all failed): not checked
                a_result = true;
                return true;
            }
            if (a_loose && fn == Func::kGetGlobalValue) {
                a_result = true;
                return true;
            }
            if (fn != Func::kGetIsID && fn != Func::kGetInFaction && fn != Func::kGetIsVoiceType) {
                return false;
            }
            const auto obj = a_c->data.object.get();
            auto       form = static_cast<RE::TESForm*>(a_c->data.functionData.params[0]);
            if (!form || form->GetFile(0) != a_mod || obj != Object::kSelf || a_c->data.flags.swapTarget) {
                return false;
            }
            float value = 0.0f;
            if (fn == Func::kGetIsID) {
                value = 1.0f;
            } else if (fn == Func::kGetInFaction) {
                // the mod's follower faction: your teammate, or in Skyrim's CurrentFollowerFaction (also while
                // waiting; user log: Sofia's "Follow" (after Wait) lines all failed here).
                // (user video 5: counting everyone as in it hid the recruit lines, which need "not in it")
                static auto current = RE::TESForm::LookupByID<RE::TESFaction>(0x0005C84E);
                const bool  in = a_speaker->IsPlayerTeammate() || (current && a_speaker->GetFactionRank(current, false) >= 0);
                value = in ? 1.0f : 0.0f;
            } else {
                auto npc = a_speaker->GetActorBase();
                value = npc && npc->voiceType == form ? 1.0f : 0.0f;
            }
            a_result = Compare(value, a_c);
            return true;
        }

        // The line's conditions hold for a_speaker talking to the player (own-mod ones as OwnModCondition says).
        // Conditions on quest aliases / package / event data can't be checked here: such lines are not used.
        // a_pass: 0 = strict, 1 = loose (see OwnModCondition), 2 = conditions not checked (last try, user video
        // 2026-11-11: "Follow me" got no line, so the vanilla answer was said without sound).
        thread_local int g_failFn = -1;  // Usable: function of the last condition that failed (for the log)

        bool Usable(RE::TESTopicInfo* a_info, RE::Actor* a_speaker, int a_pass)
        {
            const bool               loose = a_pass >= 1;
            const auto               mod = a_info->GetFile(0);
            RE::ConditionCheckParams params(a_speaker, RE::PlayerCharacter::GetSingleton());
            bool                     all = true;
            bool                     group = false;  // OR group so far
            bool                     open = false;
            for (auto c = a_info->objConditions.head; c; c = c->next) {
                bool ok = true;
                if (!OwnModCondition(c, mod, a_speaker, loose, ok)) {
                    const auto obj = c->data.object.get();
                    if (c->data.flags.usesAliases || c->data.flags.usePackData || obj == Object::kQuestAlias || obj == Object::kPackData ||
                        obj == Object::kEventData) {
                        g_failFn = 10000 + static_cast<int>(obj);
                        return false;
                    }
                    ok = a_pass >= 2 || c->IsTrue(params);
                }
                if (!ok) {
                    g_failFn = static_cast<int>(c->data.functionData.function.get());
                }
                group = group || ok;
                open = true;
                if (!c->data.flags.isOR) {  // OR binds tighter than AND: a group ends at an AND item
                    all = all && group;
                    group = false;
                    open = false;
                }
            }
            if (open) {
                all = all && group;
            }
            return all;
        }

        // The voice mod's lines that match the line a_src the game picked (see the file header).
        std::vector<const Line*> Candidates(const std::vector<Line>& a_lines, const RE::TESTopic* a_src)
        {
            std::vector<const Line*> out;
            const auto               sub = a_src->data.subtype.get();
            if (sub == Subtype::kHello || sub == Subtype::kGoodBye) {
                for (auto& l : a_lines) {
                    if (l.topic->data.subtype.get() == sub) {
                        out.push_back(&l);
                    }
                }
                return out;
            }
            if (sub != Subtype::kCustom) {
                return out;
            }
            const auto edid = EditorID(a_src);
            for (auto& k : kKeys) {
                if (!EndsWithI(edid, k.src)) {
                    continue;
                }
                for (auto d : k.dst) {
                    if (d.empty()) {
                        break;
                    }
                    // all endings together (user log: "Follow me" after Wait only tried FollowMeTopic, whose
                    // recruit lines need "not following"; FollowTopic has the ones for a waiting follower)
                    for (auto& l : a_lines) {
                        if (l.topic->data.subtype.get() == Subtype::kCustom && EndsWithI(TopicStem(EditorID(l.topic)), d)) {
                            out.push_back(&l);
                        }
                    }
                }
                break;
            }
            return out;
        }

        // For a Lazy Followers follower with a modded voice saying a_info (of a_topic): a matching line of the
        // voice's mod (see the file header), else nullptr. a_where: for the log (nullptr = no log line).
        // a_none (optional): set when it is such a follower and the voice mod has no line for it.
        const Line* Pick(RE::TESTopic* a_topic, RE::TESTopicInfo* a_info, RE::Actor* a_speaker, const char* a_where, bool* a_none = nullptr)
        {
            if (!a_topic || !a_info || !a_speaker) {
                return nullptr;
            }
            auto npc = a_speaker->GetActorBase();
            auto voice = npc ? npc->voiceType : nullptr;
            auto file = npc ? npc->GetFile(0) : nullptr;
            if (!voice || !file || !EqualsI(file->GetFilename(), "LazyFollowers.esp")) {
                return nullptr;
            }
            const std::vector<Line>* lines = nullptr;
            {
                std::lock_guard lock(g_lock);
                auto            it = g_lines.find(voice->GetFormID());
                if (it == g_lines.end()) {
                    return nullptr;  // a base-game voice, or not prepared
                }
                lines = &it->second;  // never changed or erased after Prepare
            }
            for (auto& l : *lines) {
                if (l.info == a_info) {
                    return nullptr;  // already a line of that voice
                }
            }
            std::vector<const Line*> usable;
            const auto               candidates = Candidates(*lines, a_topic);
            g_failFn = -1;
            for (int pass : { 0, 1, 2 }) {
                for (auto c : candidates) {
                    if (Usable(c->info, a_speaker, pass)) {
                        usable.push_back(c);
                    }
                }
                if (!usable.empty()) {
                    break;
                }
            }
            const auto who = a_speaker->GetDisplayFullName();
            if (usable.empty()) {
                if (a_where) {
                    logger::info("Voice ({}): {} - no line of its voice mod for topic {:08X} {} (kept {:08X}; {} candidates, last failed condition function {})",
                        a_where, who ? who : "?", a_topic->GetFormID(), EditorID(a_topic), a_info->GetFormID(), candidates.size(), g_failFn);
                }
                if (a_none) {
                    *a_none = true;
                }
                return nullptr;
            }
            thread_local std::mt19937 rng{ std::random_device{}() };
            const Line*               pick = usable[std::uniform_int_distribution<std::size_t>(0, usable.size() - 1)(rng)];
            if (a_where) {
                logger::info("Voice ({}): {} says {:08X} ({}) instead of {:08X} ({})", a_where, who ? who : "?", pick->info->GetFormID(),
                    EditorID(pick->topic), a_info->GetFormID(), EditorID(a_topic));
            }
            return pick;
        }

        // ---- lines said outside the talk menu: greetings, goodbyes (NG, user video "Voice sliently 3": swapping
        // the built item's responses showed the vanilla greeting anyway - the spoken text is read from the info
        // again). Same shared-line link as the talk menu: the picked line goes into the info's dataInfo just
        // before the game builds the line and stays ~1.5 s (the game reads it again when the line starts), then
        // the info's own dataInfo is put back. Another speaker building the same info meanwhile gets the
        // info's own data for its build (CtorHook). Any thread.
        struct SaidRedirect
        {
            RE::TESTopicInfo* own;  // the info's own dataInfo
            RE::TESTopicInfo* to;   // the voice line put in
            unsigned          gen;  // which redirect the pending put-back belongs to
        };
        std::mutex                                             g_saidLock;
        std::unordered_map<RE::TESTopicInfo*, SaidRedirect>    g_said;
        unsigned                                               g_saidGen = 0;

        // Put-backs due (g_saidLock): time -> info + the redirect it belongs to. One thread runs them (NG cleanup:
        // was a new thread per line said).
        std::multimap<std::chrono::steady_clock::time_point, std::pair<RE::TESTopicInfo*, unsigned>> g_saidDue;
        std::condition_variable                                                                    g_saidWake;

        // The put-back thread: waits for the next due entry, then gives the info its own dataInfo back unless it
        // was said again meanwhile (newer redirect).
        void SaidLoop()
        {
            std::unique_lock lock(g_saidLock);
            for (;;) {
                if (g_saidDue.empty()) {
                    g_saidWake.wait(lock);
                    continue;
                }
                const auto due = g_saidDue.begin()->first;
                if (std::chrono::steady_clock::now() < due) {
                    g_saidWake.wait_until(lock, due);
                    continue;
                }
                const auto [info, gen] = g_saidDue.begin()->second;
                g_saidDue.erase(g_saidDue.begin());
                if (auto it = g_said.find(info); it != g_said.end() && it->second.gen == gen) {
                    info->dataInfo = it->second.own;
                    g_said.erase(it);
                }
            }
        }

        // Points a_info at a_to for ~1.5 s (renewed when said again meanwhile).
        void RedirectSaid(RE::TESTopicInfo* a_info, RE::TESTopicInfo* a_to)
        {
            static std::once_flag started;
            std::call_once(started, []() { std::thread(SaidLoop).detach(); });
            {
                std::lock_guard lock(g_saidLock);
                const unsigned  gen = ++g_saidGen;
                auto            it = g_said.find(a_info);
                if (it == g_said.end()) {
                    g_said.emplace(a_info, SaidRedirect{ a_info->dataInfo, a_to, gen });
                } else {
                    it->second.to = a_to;
                    it->second.gen = gen;
                }
                a_info->dataInfo = a_to;
                g_saidDue.emplace(std::chrono::steady_clock::now() + 1500ms, std::make_pair(a_info, gen));
            }
            g_saidWake.notify_one();
        }

        // ---- talk menu (NG, user videos "Voice sliently" 1 + 2) ----
        // A talk menu answer is built when the player clicks the topic: not through the hooked DialogueItem::Ctor
        // call and not from the menu list's prepared responses (swapping those changed nothing in game). So the
        // game's own shared-line link is used: TESTopicInfo::dataInfo ("response data from another info") makes
        // the game take the text + voice file from that info. While the talk menu is open with one of our
        // followers, each listed answer points at its picked voice-mod line; on close they are put back
        // (RestoreMenu). Conditions and scripts stay the listed info's own (Lazy Followers / vanilla follow, wait).
        std::atomic<bool>                                                             g_menuOpen{ false };
        std::unordered_map<RE::TESTopicInfo*, RE::TESTopicInfo*>                      g_redirected;  // info -> its own dataInfo (g_menuLock)
        std::unordered_map<const RE::MenuTopicManager::Dialogue*, RE::TESTopicInfo*> g_checked;     // main thread: entry -> info checked
        std::recursive_mutex                                                          g_menuLock;    // g_redirected (MenuDataHook may run off the main thread)

        // Puts every redirected answer back (talk menu closed).
        void RestoreMenu()
        {
            std::lock_guard lock(g_menuLock);
            for (auto& [info, own] : g_redirected) {
                info->dataInfo = own;
            }
            if (!g_redirected.empty()) {
                logger::info("Voice (menu): {} answers put back", g_redirected.size());
            }
            g_redirected.clear();
            g_checked.clear();
        }

        // Main thread, talk menu open: new list entries of our follower get their answer redirected.
        void PatchMenu()
        {
            auto mtm = RE::MenuTopicManager::GetSingleton();
            if (!mtm || !g_menuOpen.load()) {
                return;
            }
            auto cs = reinterpret_cast<LPCRITICAL_SECTION>(&mtm->criticalSection);
            ::EnterCriticalSection(cs);
            std::unique_lock lock(g_menuLock);
            try {
                auto ref = mtm->speaker.get();
                auto speaker = ref ? ref->As<RE::Actor>() : nullptr;
                if (speaker && mtm->dialogueList) {
                    for (auto d : *mtm->dialogueList) {
                        RenameMenuEntry(d);  // NG 1.2: nickname of a real NPC follower
                    }
                }
                if (speaker && mtm->dialogueList) {
                    for (auto d : *mtm->dialogueList) {
                        if (!d || !d->parentTopicInfo || !d->parentTopic) {
                            continue;
                        }
                        auto info = d->parentTopicInfo;
                        if (auto it = g_checked.find(d); it != g_checked.end() && it->second == info) {
                            continue;  // checked already
                        }
                        g_checked[d] = info;
                        if (g_redirected.contains(info)) {
                            continue;
                        }
                        if (auto pick = Pick(d->parentTopic, info, speaker, "menu")) {
                            g_redirected.emplace(info, info->dataInfo);
                            info->dataInfo = pick->info;
                            // the game picks the topic's answer again on click (user video "Voice sliently 4": Trade
                            // listed a vanilla answer, an RDO one was said): every answer of the topic is redirected
                            auto topic = d->parentTopic;
                            int  more = 0;
                            for (std::uint32_t i = 0; topic->topicInfos && i < topic->numTopicInfos; ++i) {
                                auto other = topic->topicInfos[i];
                                if (!other || other == info || g_redirected.contains(other)) {
                                    continue;
                                }
                                if (auto p2 = Pick(topic, other, speaker, nullptr)) {
                                    g_redirected.emplace(other, other->dataInfo);
                                    other->dataInfo = p2->info;
                                    ++more;
                                }
                            }
                            if (more) {
                                logger::info("Voice (menu): + {} other answers of topic {:08X} redirected", more, topic->GetFormID());
                            }
                        }
                    }
                }
            } catch (...) {
            }
            lock.unlock();
            ::LeaveCriticalSection(cs);
        }

        // ---- talk menu builders (NG, game-code dump 2026-11-11): the talk menu builds its entries in
        // MenuTopicManager's Dialogue ctor (ID 35254, also the greeting when talking starts) and rebuilds an
        // answer on click (ID 35256). Both read the info's text + voice through the response-data getter
        // (ID 25626, follows dataInfo). Their getter calls are hooked: the info is redirected right before it is
        // read, so a greeting built as the menu opens is no longer missed by the 50 ms PatchMenu loop.
        std::uintptr_t g_getData = 0;  // the original getter (call target at the hooked sites)

        // Getter call in the two talk-menu builders: our follower (the talk menu's speaker) -> a_info points at
        // its picked voice line first (put back by RestoreMenu when the talk menu closes).
        void* MenuDataHook(RE::TESTopicInfo* a_info, void* a_out)
        {
            try {
                auto mtm = RE::MenuTopicManager::GetSingleton();
                auto ref = mtm ? mtm->speaker.get() : RE::NiPointer<RE::TESObjectREFR>{};
                auto speaker = ref ? ref->As<RE::Actor>() : nullptr;
                if (a_info && a_info->parentTopic && speaker) {
                    std::lock_guard lock(g_menuLock);
                    if (!g_redirected.contains(a_info)) {
                        if (auto pick = Pick(a_info->parentTopic, a_info, speaker, "menu build")) {
                            g_redirected.emplace(a_info, a_info->dataInfo);
                            a_info->dataInfo = pick->info;
                        }
                    }
                }
            } catch (...) {
            }
            return reinterpret_cast<void* (*)(RE::TESTopicInfo*, void*)>(g_getData)(a_info, a_out);
        }

        // NG 1.2: the talk menu's entry ctor (ID 35254, 6 arguments) and answer rebuild (ID 35256, 5 arguments, from
        // the game-code dump): the game's function, then the nickname of a real NPC follower in the entry.
        std::uintptr_t g_entryCtor = 0;
        std::uintptr_t g_entryBuild = 0;

        void* EntryCtorHook(void* a_entry, void* a_2, void* a_3, void* a_4, void* a_5, std::uint8_t a_6)
        {
            auto result = reinterpret_cast<void* (*)(void*, void*, void*, void*, void*, std::uint8_t)>(g_entryCtor)(a_entry, a_2, a_3, a_4, a_5, a_6);
            try {
                RenameMenuEntry(static_cast<RE::MenuTopicManager::Dialogue*>(a_entry));
            } catch (...) {
            }
            return result;
        }

        void* EntryBuildHook(void* a_entry, void* a_2, void* a_3, void* a_4, void* a_5)
        {
            auto result = reinterpret_cast<void* (*)(void*, void*, void*, void*, void*)>(g_entryBuild)(a_entry, a_2, a_3, a_4, a_5);
            try {
                RenameMenuEntry(static_cast<RE::MenuTopicManager::Dialogue*>(a_entry));
            } catch (...) {
            }
            return result;
        }

        // Runs PatchMenu on the main thread ~20 times a second while the talk menu is open.
        void MenuLoop()
        {
            while (g_menuOpen.load()) {
                SKSE::GetTaskInterface()->AddTask([]() { PatchMenu(); });
                std::this_thread::sleep_for(50ms);
            }
        }

        // DialogueMenu open / close: starts / stops MenuLoop.
        class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (a_event && a_event->menuName == RE::DialogueMenu::MENU_NAME) {
                    if (a_event->opening) {
                        if (!g_menuOpen.exchange(true)) {
                            // no RestoreMenu here: the greeting was already redirected by MenuDataHook
                            std::thread(MenuLoop).detach();
                        }
                    } else {
                        g_menuOpen = false;
                        SKSE::GetTaskInterface()->AddTask([]() { RestoreMenu(); });
                    }
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        };
        MenuSink g_menuSink;

        // ---- the voice mod's own talk options (NG, user 2026-11-11: "add the dialogue options from Sofia's esp") ----
        // The voice mod's lines check "is the speaker that mod's NPC" (GetIsID / GetInFaction / GetIsVoiceType on its
        // own forms), so they never show for our follower. Those condition items of the voice mod's lines that have
        // no script are answered for our follower like OwnModCondition says (TESConditionItem::IsTrue calls
        // hooked), so its chat options (e.g. Sofia's "Can I talk to you?" / "How are you feeling?") show and play.
        // Lines with a script stay hidden: their scripts act on the mod's quest aliases = the real NPC (romance,
        // wedding, gifts, storage, bard songs). Lines of the swapped follower topics (kKeys: follow, wait, trade,
        // part ways, favor) stay hidden too (they would show twice and do nothing). Lines whose choices all lead to
        // hidden topics stay hidden (no dead ends). Which lines have a script / choices: read from the plugin file.
        std::shared_mutex                                                  g_unlockLock;
        struct Unlocked
        {
            RE::BGSVoiceType* voice;  // the voice it is changed for
            bool              block;  // false: answered as for the mod's NPC; true: always false (a hidden line)
        };
        std::unordered_map<const RE::TESConditionItem*, Unlocked> g_unlock;  // condition item -> how it is answered for our follower
        std::atomic<bool>                                                  g_anyUnlock{ false };
        std::uintptr_t                                                     g_isTrue = 0;  // the game's TESConditionItem::IsTrue

        // One INFO record of the plugin file: has a script (VMAD), its choices (TCLT, runtime FormIDs).
        struct EspInfo
        {
            bool                    scripted = false;
            std::vector<RE::FormID> choices;
        };

        // What Voice needs from a plugin file: its INFO records and the voice type of each of its NPCs as the file
        // says (runtime FormIDs; the game's voice of an NPC can be changed later, see NamesVoice).
        struct EspData
        {
            std::unordered_map<RE::FormID, EspInfo>    infos;
            std::unordered_map<RE::FormID, RE::FormID> npcVoices;  // NPC_ -> VTCK
        };

        // Reads a_file (Data\<name>, through MO2's virtual folder). Compressed records are unpacked (zlib).
        // a_npcsOnly (NG): only the NPC_ records' voices (the owner scan; INFO records are skipped).
        EspData ReadPlugin(const RE::TESFile* a_file, bool a_npcsOnly = false)
        {
            EspData           out;
            const std::string name(a_file->GetFilename());
            std::ifstream     in(std::filesystem::path("Data") / name, std::ios::binary);
            if (!in) {
                logger::warn("Voice: could not read Data\\{} - its talk options are not added", name);
                return out;
            }
            const std::vector<char> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto rd16 = [](const char* a_p) { std::uint16_t v; std::memcpy(&v, a_p, 2); return v; };
            auto rd32 = [](const char* a_p) { std::uint32_t v; std::memcpy(&v, a_p, 4); return v; };
            if (d.size() < 24 || std::memcmp(d.data(), "TES4", 4) != 0) {
                return out;
            }
            auto dh = RE::TESDataHandler::GetSingleton();
            // the file's masters (TES4 MAST), then its own records' index
            std::vector<std::string> masters;
            const std::size_t        headEnd = 24 + rd32(d.data() + 4);
            for (std::size_t o = 24; o + 6 <= headEnd && o + 6 <= d.size();) {
                const auto sz = rd16(d.data() + o + 4);
                if (std::memcmp(d.data() + o, "MAST", 4) == 0 && o + 6 + sz <= d.size()) {
                    masters.emplace_back(d.data() + o + 6);
                }
                o += 6 + sz;
            }
            // a FormID as written in the file -> the runtime FormID (0 = not loaded)
            auto resolve = [&](std::uint32_t a_raw) -> RE::FormID {
                const auto  idx = a_raw >> 24;
                const auto& mod = idx < masters.size() ? masters[idx] : name;
                auto        file = dh ? dh->LookupModByName(mod) : nullptr;
                return file ? dh->LookupFormID(a_raw & (file->IsLight() ? 0xFFF : 0xFFFFFF), mod) : 0;
            };
            // calls a_fn(type, data, size) for each subrecord of a record body
            auto subrecords = [&](const char* a_body, std::size_t a_size, auto&& a_fn) {
                std::uint32_t big = 0;
                for (std::size_t s = 0; s + 6 <= a_size;) {
                    std::size_t sz = rd16(a_body + s + 4);
                    if (std::memcmp(a_body + s, "XXXX", 4) == 0) {
                        big = rd32(a_body + s + 6);
                        s += 6 + sz;
                        continue;
                    }
                    if (big) {
                        sz = big;
                        big = 0;
                    }
                    if (s + 6 + sz > a_size) {
                        break;
                    }
                    a_fn(a_body + s, a_body + s + 6, sz);
                    s += 6 + sz;
                }
            };
            std::vector<char> unpacked;
            for (std::size_t o = headEnd; o + 24 <= d.size();) {
                const auto size = rd32(d.data() + o + 4);
                if (std::memcmp(d.data() + o, "GRUP", 4) == 0) {
                    o += 24;  // a group: its records follow its header
                    continue;
                }
                const auto end = o + 24 + size;
                if (end > d.size()) {
                    break;
                }
                const bool isInfo = !a_npcsOnly && std::memcmp(d.data() + o, "INFO", 4) == 0;
                const bool isNpc = std::memcmp(d.data() + o, "NPC_", 4) == 0;
                if (isInfo || isNpc) {
                    const char* body = d.data() + o + 24;
                    std::size_t bodySize = size;
                    bool        ok = true;
                    if (rd32(d.data() + o + 8) & 0x40000) {  // compressed: 4 bytes unpacked size + zlib data
                        ok = false;
                        if (size >= 4) {
                            uLongf len = rd32(body);
                            unpacked.resize(len);
                            if (uncompress(reinterpret_cast<Bytef*>(unpacked.data()), &len, reinterpret_cast<const Bytef*>(body + 4), size - 4) == Z_OK) {
                                body = unpacked.data();
                                bodySize = len;
                                ok = true;
                            }
                        }
                    }
                    const auto id = resolve(rd32(d.data() + o + 12));
                    if (isInfo) {
                        EspInfo info;
                        info.scripted = !ok;  // could not unpack: counted as scripted (stays hidden)
                        if (ok) {
                            subrecords(body, bodySize, [&](const char* a_type, const char* a_data, std::size_t a_sz) {
                                if (std::memcmp(a_type, "VMAD", 4) == 0) {
                                    info.scripted = true;
                                } else if (std::memcmp(a_type, "TCLT", 4) == 0 && a_sz >= 4) {
                                    info.choices.push_back(resolve(rd32(a_data)));
                                }
                            });
                        }
                        if (id) {
                            out.infos[id] = std::move(info);
                        }
                    } else if (ok && id) {
                        subrecords(body, bodySize, [&](const char* a_type, const char* a_data, std::size_t a_sz) {
                            if (std::memcmp(a_type, "VTCK", 4) == 0 && a_sz >= 4) {
                                out.npcVoices[id] = resolve(rd32(a_data));
                            }
                        });
                    }
                }
                o = end;
            }
            return out;
        }

        // A global of the voice mod that says "has met the player" (Editor ID contains MetPlayer, e.g. JJSofiaHasMetPlayer).
        bool MetPlayerGlobal(const RE::TESForm* a_form)
        {
            auto        global = a_form->As<RE::TESGlobal>();
            const char* e = global ? global->GetFormEditorID() : nullptr;
            if (!e) {
                return false;
            }
            std::string s(e);
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s.find("metplayer") != std::string::npos;
        }

        // A line of a swapped follower topic (kKeys endings of TopicStem: FollowMe, Wait, Trade...).
        bool SwappedTopic(const RE::TESTopic* a_topic)
        {
            const auto edid = TopicStem(EditorID(a_topic));
            for (auto& k : kKeys) {
                for (auto e : k.dst) {
                    if (!e.empty() && EndsWithI(edid, e)) {
                        return true;
                    }
                }
            }
            return false;
        }

        // Picks the voice mod's lines that are shown for our followers (see above) and records their own-mod
        // condition items in g_unlock. Main thread (Prepare).
        void Unlock(RE::BGSVoiceType* a_voice, const std::vector<Line>& a_lines, const EspData& a_esp)
        {
            const auto  mod = a_voice->GetFile(0);
            const auto& esp = a_esp.infos;
            if (!mod || esp.empty()) {
                return;
            }
            std::unordered_set<RE::TESTopicInfo*> shown;
            for (auto& l : a_lines) {
                auto it = esp.find(l.info->GetFormID());
                if (l.info->GetFile(0) == mod && it != esp.end() && !it->second.scripted && !SwappedTopic(l.topic)) {
                    shown.insert(l.info);
                }
            }
            // no dead ends: a line whose choices all lead to topics of the mod with no shown line is hidden too
            for (bool changed = true; changed;) {
                changed = false;
                std::unordered_set<const RE::TESTopic*> open;
                for (auto info : shown) {
                    open.insert(info->parentTopic);
                }
                for (auto it = shown.begin(); it != shown.end();) {
                    const auto& choices = esp.at((*it)->GetFormID()).choices;
                    bool        dead = !choices.empty();
                    for (auto id : choices) {
                        auto topic = RE::TESForm::LookupByID<RE::TESTopic>(id);
                        if (topic && (topic->GetFile(0) != mod || open.contains(topic))) {
                            dead = false;
                            break;
                        }
                    }
                    if (dead) {
                        it = shown.erase(it);
                        changed = true;
                    } else {
                        ++it;
                    }
                }
            }
            {
                std::unique_lock lock(g_unlockLock);
                for (auto info : shown) {
                    for (auto c = info->objConditions.head; c; c = c->next) {
                        const auto fn = c->data.functionData.function.get();
                        auto       form = static_cast<RE::TESForm*>(c->data.functionData.params[0]);
                        if ((fn == Func::kGetIsID || fn == Func::kGetInFaction || fn == Func::kGetIsVoiceType) && form &&
                            form->GetFile(0) == mod && c->data.object.get() == Object::kSelf && !c->data.flags.swapTarget) {
                            g_unlock[c] = { a_voice, false };
                        } else if (fn == Func::kGetGlobalValue && form && form->GetFile(0) == mod && MetPlayerGlobal(form)) {
                            // "has met the player" (user video 2026-11-11 07-52-42: Sofia's "Can I talk to you?" needs
                            // JJSofiaHasMetPlayer == 1, 0 while the player never met the real Sofia): our follower
                            // has met the player, so it counts as true (answered by OwnModCondition's loose rule)
                            g_unlock[c] = { a_voice, false };
                        }
                    }
                }
                // the quests of the shown lines: their dialogue conditions (on every line of the quest; user video
                // 2026-11-11 08-00-43: Sofia's quest JJSofiaDialogue has GetIsID JJSofiaFollower) are answered the
                // same way, and every other line of those quests is blocked for our follower (it would show now:
                // romance, storage... have scripts acting on the real NPC). A line is blocked by one of its
                // condition items that is not in an OR group (always false for our follower).
                std::unordered_set<const RE::TESQuest*> quests;
                for (auto info : shown) {
                    auto q = info->parentTopic ? info->parentTopic->ownerQuest : nullptr;
                    if (q && q->GetFile(0) == mod && quests.insert(q).second) {
                        for (auto c = q->objConditions.head; c; c = c->next) {
                            const auto fn = c->data.functionData.function.get();
                            auto       form = static_cast<RE::TESForm*>(c->data.functionData.params[0]);
                            if ((fn == Func::kGetIsID || fn == Func::kGetInFaction || fn == Func::kGetIsVoiceType) && form &&
                                form->GetFile(0) == mod && c->data.object.get() == Object::kSelf && !c->data.flags.swapTarget) {
                                g_unlock[c] = { a_voice, false };
                            }
                        }
                    }
                }
                std::size_t blocked = 0, open = 0;
                if (auto dh = RE::TESDataHandler::GetSingleton(); dh && !quests.empty()) {
                    for (auto topic : dh->GetFormArray<RE::TESTopic>()) {
                        if (!topic || !topic->topicInfos || !quests.contains(topic->ownerQuest)) {
                            continue;
                        }
                        for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
                            auto info = topic->topicInfos[i];
                            if (!info || shown.contains(info)) {
                                continue;
                            }
                            RE::TESConditionItem* block = nullptr;
                            bool                  prevOr = false;
                            for (auto c = info->objConditions.head; c && !block; c = c->next) {
                                if (!c->data.flags.isOR && !prevOr) {
                                    block = c;  // a group of its own: false hides the line
                                }
                                prevOr = c->data.flags.isOR;
                            }
                            if (block) {
                                g_unlock[block] = { a_voice, true };
                                ++blocked;
                            } else {
                                ++open;
                            }
                        }
                    }
                }
                g_anyUnlock = !g_unlock.empty();
                logger::info("Voice {:08X}: {} quest(s) of its talk lines opened, {} other lines of them kept hidden ({} without a condition to hide them)",
                    a_voice->GetFormID(), quests.size(), blocked, open);
            }
            logger::info("Voice {:08X}: {} of its {} lines shown as talk options for Lazy Followers followers (the rest: script, follower topic or dead end)",
                a_voice->GetFormID(), shown.size(), a_lines.size());
        }

        // Replaces the game's calls of TESConditionItem::IsTrue: an unlocked condition item (see above) checked on
        // our follower with that voice is answered by OwnModCondition (a blocking one: false), everything else by
        // the game.
        bool IsTrueHook(const RE::TESConditionItem* a_item, RE::ConditionCheckParams& a_params)
        {
            using func_t = bool (*)(const RE::TESConditionItem*, RE::ConditionCheckParams&);
            if (g_anyUnlock.load(std::memory_order_relaxed) && a_item) {
                RE::BGSVoiceType* voice = nullptr;
                bool              block = false;
                {
                    std::shared_lock lock(g_unlockLock);
                    if (auto it = g_unlock.find(a_item); it != g_unlock.end()) {
                        voice = it->second.voice;
                        block = it->second.block;
                    }
                }
                if (voice) {
                    try {
                        auto actor = a_params.actionRef ? a_params.actionRef->As<RE::Actor>() : nullptr;
                        auto npc = actor ? actor->GetActorBase() : nullptr;
                        auto file = npc ? npc->GetFile(0) : nullptr;
                        bool result = false;
                        if (npc && npc->voiceType == voice && file && EqualsI(file->GetFilename(), "LazyFollowers.esp")) {
                            if (block) {
                                return false;  // a hidden line of an opened quest
                            }
                            if (OwnModCondition(a_item, voice->GetFile(0), actor, a_item->data.functionData.function.get() == Func::kGetGlobalValue, result)) {
                                return result;
                            }
                        }
                    } catch (...) {
                    }
                }
            }
            return reinterpret_cast<func_t>(g_isTrue)(a_item, a_params);
        }

        // ---- does a line have audio for the speaker's voice? (NG, user 2026-11-11: voice packs that recorded the
        // vanilla lines under their own voice type have no lines of their own, so CtorHook silenced lines that do
        // have sound.) The game's own constructor builds the line once for that speaker (its responses hold the
        // voice file path the game will play); the file is looked up like the game does (loose or BSA). The
        // built item is kept (a few bytes) and the answer cached per line + voice.
        std::mutex                                                                  g_audioLock;
        std::map<std::pair<const RE::TESTopicInfo*, const RE::BGSVoiceType*>, bool> g_audio;

        // A voice file of a response: the path the game wrote, tried as .fuz / .xwm / .wav (the game plays any).
        bool VoiceFileExists(std::string a_path)
        {
            if (a_path.empty()) {
                return false;
            }
            // Fuz Ro D-oh puts its own silent file in when the line has none (user log 2026-11-11: "Data\Sound\Voice\
            // Fuz Ro Doh\Stock_1.xwm" for vanilla lines said with Sofia's / Severa's voice)
            {
                std::string lower(a_path);
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("fuz ro doh") != std::string::npos) {
                    return false;
                }
            }
            if (a_path.size() > 5 && EqualsI(std::string_view(a_path).substr(0, 5), "data\\")) {
                a_path.erase(0, 5);
            }
            const auto dot = a_path.find_last_of('.');
            const auto stem = dot == std::string::npos ? a_path : a_path.substr(0, dot);
            for (auto ext : { ".fuz", ".xwm", ".wav" }) {
                RE::BSResourceNiBinaryStream stream(stem + ext);
                if (stream.good()) {
                    return true;
                }
            }
            return false;
        }

        // a_info said by a_speaker has a voice file for its voice (true too when the line has no responses).
        bool HasAudio(RE::TESQuest* a_quest, RE::TESTopic* a_topic, RE::TESTopicInfo* a_info, RE::Actor* a_speaker)
        {
            auto npc = a_speaker->GetActorBase();
            const auto key = std::make_pair(static_cast<const RE::TESTopicInfo*>(a_info), static_cast<const RE::BGSVoiceType*>(npc ? npc->voiceType : nullptr));
            {
                std::lock_guard lock(g_audioLock);
                if (auto it = g_audio.find(key); it != g_audio.end()) {
                    return it->second;
                }
            }
            auto        item = new RE::DialogueItem(a_quest, a_topic, a_info, a_speaker);  // the game's own constructor (not a hooked call)
            bool        found = item->responses.empty();
            std::string path;
            for (auto r : item->responses) {
                if (r && r->voice.c_str()) {
                    path = r->voice.c_str();
                    if (VoiceFileExists(path)) {
                        found = true;
                        break;
                    }
                }
            }
            const auto who = a_speaker->GetDisplayFullName();
            logger::info("Voice (said): {} - line {:08X} {} ({})", who ? who : "?", a_info->GetFormID(), found ? "has audio for its voice, kept" : "has no audio for its voice",
                path.empty() ? "no voice file path" : path);
            std::lock_guard lock(g_audioLock);
            g_audio[key] = found;
            return found;
        }

        // DialogueItem::Ctor for CtorHook: our follower with a modded voice -> the info points at the picked voice
        // line (RedirectSaid) before the game builds it; anyone else building an info we redirected gets the
        // info's own data for that build.
        RE::DialogueItem* BuildItem(RE::DialogueItem* a_this, RE::TESQuest* a_quest, RE::TESTopic* a_topic, RE::TESTopicInfo* a_info, RE::Actor* a_speaker)
        {
            using func_t = RE::DialogueItem* (*)(RE::DialogueItem*, RE::TESQuest*, RE::TESTopic*, RE::TESTopicInfo*, RE::Actor*);
            const Line* pick = nullptr;
            bool        none = false;
            try {
                pick = Pick(a_topic, a_info, a_speaker, "said", &none);
            } catch (...) {
            }
            if (pick) {
                RedirectSaid(a_info, pick->info);
                return reinterpret_cast<func_t>(g_ctor)(a_this, a_quest, a_topic, a_info, a_speaker);
            }
            bool silent = false;
            if (none) {
                try {
                    silent = !HasAudio(a_quest, a_topic, a_info, a_speaker);
                } catch (...) {
                }
            }
            if (silent) {
                // no line of the voice mod and no voice file for this voice: the follower would move her lips
                // with a subtitle and nothing heard (user video 2026-11-11). Said with an empty line instead:
                // nothing shown, nothing said.
                static auto empty = RE::TESForm::LookupByID<RE::TESTopicInfo>(0x0010F653);  // Skyrim.esm INFO, no responses
                if (empty) {
                    logger::info("Voice (said): {:08X} silenced", a_info->GetFormID());
                    RedirectSaid(a_info, empty);
                    return reinterpret_cast<func_t>(g_ctor)(a_this, a_quest, a_topic, a_info, a_speaker);
                }
            }
            std::unique_lock lock(g_saidLock);
            auto             it = a_info ? g_said.find(a_info) : g_said.end();
            if (it == g_said.end()) {
                lock.unlock();
                return reinterpret_cast<func_t>(g_ctor)(a_this, a_quest, a_topic, a_info, a_speaker);
            }
            a_info->dataInfo = it->second.own;  // another speaker: its own line (lock held during the build)
            auto result = reinterpret_cast<func_t>(g_ctor)(a_this, a_quest, a_topic, a_info, a_speaker);
            a_info->dataInfo = it->second.to;
            return result;
        }

        // Replaces every call of DialogueItem::Ctor: BuildItem, then (NG 1.2) the nickname of a real NPC follower
        // in the line's text (its subtitles).
        RE::DialogueItem* CtorHook(RE::DialogueItem* a_this, RE::TESQuest* a_quest, RE::TESTopic* a_topic, RE::TESTopicInfo* a_info, RE::Actor* a_speaker)
        {
            auto result = BuildItem(a_this, a_quest, a_topic, a_info, a_speaker);
            try {
                std::string from, to;
                if (result && a_speaker && LF::Nickname(a_speaker->GetFormID(), from, to)) {
                    RenameResponses(result->responses, from, to);
                }
            } catch (...) {
            }
            return result;
        }
    }

    // The talk menu watcher (PatchMenu) + the direct calls / jumps to DialogueItem::Ctor in the game's code
    // pointed at CtorHook.
    void Install()
    {
        static bool done = false;
        if (done) {
            return;
        }
        done = true;
        if (auto ui = RE::UI::GetSingleton()) {
            ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_menuSink);  // talk menu answers (PatchMenu)
        }
        REL::Relocation<std::uintptr_t> ctor{ RELOCATION_ID(34413, 35220) };
        g_ctor = ctor.address();
        const auto  text = REL::Module::get().segment(REL::Segment::textx);
        const auto  start = text.address();
        const auto* bytes = text.pointer<const std::uint8_t>();
        std::vector<std::pair<std::uintptr_t, bool>> sites;  // address, is a jmp
        for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
            if (bytes[i] != 0xE8 && bytes[i] != 0xE9) {
                continue;
            }
            std::int32_t rel;
            std::memcpy(&rel, bytes + i + 1, sizeof(rel));
            const auto dst = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(start + i + 5) + rel);
            if (dst == g_ctor) {
                sites.emplace_back(start + i, bytes[i] == 0xE9);
            }
        }
        if (sites.empty()) {
            logger::warn("Voice: no calls of DialogueItem::Ctor found - modded voice lines are not swapped");
            return;
        }
        SKSE::AllocTrampoline(sites.size() * 14);
        auto& tr = SKSE::GetTrampoline();
        for (auto [at, jmp] : sites) {
            if (jmp) {
                tr.write_branch<5>(at, CtorHook);
            } else {
                tr.write_call<5>(at, CtorHook);
            }
        }
        logger::info("Voice: hooked {} calls of DialogueItem::Ctor", sites.size());

        // the talk-menu builders' getter calls (offsets from the game-code dump of 1.6.1170). NG fix: the DLL uses
        // Address Library and loads on any AE version, where these offsets can land on another call - so only on
        // 1.6.1170, and only when both sites are calls to one and the same function (else nothing is patched).
        const auto runtime = REL::Module::get().version();
        if (runtime != REL::Version(1, 6, 1170, 0)) {
            logger::info("Voice: game {} is not 1.6.1170 - talk menu hooks left out (greetings / answers in the talk menu stay vanilla)",
                runtime.string("."sv));
        } else {
            const std::pair<std::uint64_t, std::size_t> calls[] = { { 35254, 0x26A }, { 35256, 0x3D } };
            std::uintptr_t                              getterSites[2]{};
            std::uintptr_t                              target = 0;
            bool                                        ok = true;
            for (std::size_t k = 0; k < 2; ++k) {
                const auto [id, off] = calls[k];
                getterSites[k] = REL::ID(id).address() + off;
                const auto* p = reinterpret_cast<const std::uint8_t*>(getterSites[k]);
                std::int32_t rel;
                std::memcpy(&rel, p + 1, sizeof(rel));
                const auto dst = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(getterSites[k] + 5) + rel);
                if (p[0] != 0xE8 || (target && dst != target)) {
                    logger::warn("Voice: talk menu builder {} +{:X} is not the expected call - talk menu hooks left out", id, off);
                    ok = false;
                    break;
                }
                target = dst;
            }
            int hooked = 0;
            for (std::size_t k = 0; ok && k < 2; ++k) {
                SKSE::AllocTrampoline(14);
                g_getData = SKSE::GetTrampoline().write_call<5>(getterSites[k], MenuDataHook);
                ++hooked;
            }
            logger::info("Voice: hooked {} talk menu builder calls", hooked);

            // NG 1.2: every call of the entry ctor / answer rebuild -> the wrappers (nickname)
            g_entryCtor = REL::ID(35254).address();
            g_entryBuild = REL::ID(35256).address();
            std::vector<std::pair<std::uintptr_t, bool>> entrySites;  // address, calls the ctor (else the rebuild)
            for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
                if (bytes[i] != 0xE8) {
                    continue;
                }
                std::int32_t rel;
                std::memcpy(&rel, bytes + i + 1, sizeof(rel));
                const auto dst = static_cast<std::uintptr_t>(static_cast<std::intptr_t>(start + i + 5) + rel);
                if (dst == g_entryCtor || dst == g_entryBuild) {
                    entrySites.emplace_back(start + i, dst == g_entryCtor);
                }
            }
            if (!entrySites.empty()) {
                SKSE::AllocTrampoline(entrySites.size() * 14);
            }
            for (auto [at, ctorCall] : entrySites) {
                if (ctorCall) {
                    SKSE::GetTrampoline().write_call<5>(at, EntryCtorHook);
                } else {
                    SKSE::GetTrampoline().write_call<5>(at, EntryBuildHook);
                }
            }
            logger::info("Voice: hooked {} talk menu entry calls (nicknames)", entrySites.size());
        }

        // TESConditionItem::IsTrue's calls (the voice mod's talk options, see Unlock)
        REL::Relocation<std::uintptr_t> isTrue{ RELOCATION_ID(29090, 29924) };
        g_isTrue = isTrue.address();
        std::vector<std::uintptr_t> condSites;
        for (std::size_t i = 0; i + 5 <= text.size(); ++i) {
            if (bytes[i] != 0xE8) {
                continue;
            }
            std::int32_t rel;
            std::memcpy(&rel, bytes + i + 1, sizeof(rel));
            if (static_cast<std::uintptr_t>(static_cast<std::intptr_t>(start + i + 5) + rel) == g_isTrue) {
                condSites.push_back(start + i);
            }
        }
        if (!condSites.empty()) {
            SKSE::AllocTrampoline(condSites.size() * 14);
            for (auto at : condSites) {
                SKSE::GetTrampoline().write_call<5>(at, IsTrueHook);
            }
        }
        logger::info("Voice: hooked {} calls of TESConditionItem::IsTrue (voice mod talk options)", condSites.size());
    }

    // Collects the lines of a_voice's mod (infos that name the voice, see NamesVoice). Base-game voices: none.
    void Prepare(RE::BGSVoiceType* a_voice)
    {
        if (!a_voice || BaseGameFile(a_voice->GetFile(0))) {
            return;
        }
        {
            std::lock_guard lock(g_lock);
            if (g_lines.contains(a_voice->GetFormID())) {
                return;
            }
        }
        auto dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            return;
        }
        // the voice's plugin: its NPCs with this voice as the file says (the game's may differ, see NamesVoice)
        const auto                     mod = a_voice->GetFile(0);
        const EspData                  esp = mod ? ReadPlugin(mod) : EspData{};
        std::unordered_set<RE::FormID> fileNpcs;
        for (auto& [npcId, voiceId] : esp.npcVoices) {
            if (voiceId != a_voice->GetFormID()) {
                continue;
            }
            fileNpcs.insert(npcId);
            auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcId);
            if (npc && npc->voiceType != a_voice) {
                const char* e = npc->GetFormEditorID();
                logger::info("Voice {:08X}: NPC {:08X} {} has this voice in its plugin but {:08X} in the game - its lines are counted", a_voice->GetFormID(),
                    npcId, e ? e : "", npc->voiceType ? npc->voiceType->GetFormID() : 0);
            }
        }
        std::vector<Line> lines;
        for (auto topic : dh->GetFormArray<RE::TESTopic>()) {
            if (!topic || !topic->topicInfos) {
                continue;
            }
            for (std::uint32_t i = 0; i < topic->numTopicInfos; ++i) {
                auto info = topic->topicInfos[i];
                if (!info) {
                    continue;
                }
                for (auto c = info->objConditions.head; c; c = c->next) {
                    if (NamesVoice(c, a_voice, &fileNpcs)) {
                        lines.push_back({ topic, info });
                        break;
                    }
                }
            }
        }
        logger::info("Voice {:08X}: {} lines of its mod found (used for Lazy Followers followers with it)", a_voice->GetFormID(), lines.size());
        Unlock(a_voice, lines, esp);  // its talk options
        std::lock_guard lock(g_lock);
        g_lines.emplace(a_voice->GetFormID(), std::move(lines));
        g_owners[a_voice->GetFormID()].assign(fileNpcs.begin(), fileNpcs.end());
    }

    bool IsModded(const RE::BGSVoiceType* a_voice) { return a_voice && !BaseGameFile(a_voice->GetFile(0)); }

    namespace
    {
        std::mutex                     g_ownedLock;
        std::unordered_set<RE::FormID> g_fileOwned;  // modded voices a unique NPC has in a plugin file (owner scan)
    }

    // NG (user: the Override box must also show for Severa's voice, whose NPC has another voice in the game): every
    // plugin that defines a modded voice type is read once (NPC records only, like the plugin read Apply uses to find
    // the voice's NPC); a voice a unique NPC has there (not a Lazy Followers slot) counts as a modded follower's voice.
    // Runs on its own thread (file reads); the result is there for the next list rebuild.
    void StartOwnerScan()
    {
        static std::once_flag once;
        std::call_once(once, []() {
            std::thread([]() {
                try {
                    const auto start = std::chrono::steady_clock::now();
                    auto       dh = RE::TESDataHandler::GetSingleton();
                    if (!dh) {
                        return;
                    }
                    std::unordered_set<const RE::TESFile*> files;
                    for (auto v : dh->GetFormArray<RE::BGSVoiceType>()) {
                        if (IsModded(v)) {
                            files.insert(v->GetFile(0));
                        }
                    }
                    std::unordered_set<RE::FormID> owned;
                    for (auto file : files) {
                        const auto esp = ReadPlugin(file, true);
                        for (auto& [npcId, voiceId] : esp.npcVoices) {
                            auto npc = RE::TESForm::LookupByID<RE::TESNPC>(npcId);
                            auto voice = RE::TESForm::LookupByID<RE::BGSVoiceType>(voiceId);
                            auto npcFile = npc ? npc->GetFile(0) : nullptr;
                            if (npc && npc->IsUnique() && IsModded(voice) && !(npcFile && EqualsI(npcFile->GetFilename(), "LazyFollowers.esp"))) {
                                owned.insert(voiceId);
                            }
                        }
                    }
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    logger::info("Voice: {} plugins with modded voices read in {} ms - {} voices belong to a modded NPC there", files.size(), ms, owned.size());
                    std::lock_guard lock(g_ownedLock);
                    g_fileOwned = std::move(owned);
                } catch (...) {
                    logger::warn("Voice: plugin scan for modded follower voices failed");
                }
            }).detach();
        });
    }

    std::unordered_set<RE::FormID> FileOwnedVoices()
    {
        std::lock_guard lock(g_ownedLock);
        return g_fileOwned;
    }

    std::vector<RE::FormID> PluginNPCs(RE::BGSVoiceType* a_voice)
    {
        if (!IsModded(a_voice)) {
            return {};
        }
        Prepare(a_voice);
        std::lock_guard lock(g_lock);
        auto            it = g_owners.find(a_voice->GetFormID());
        return it != g_owners.end() ? it->second : std::vector<RE::FormID>{};
    }
}
