// Menu - draws the "Lazy Followers" pages in SKSE Menu Framework. They replace the MCM's pages
// (Main, Stats, Spells, Combat, Debug, SaveLoad; Shouts is left out - see Docs). NG 1.1: the old
// "Presets & Files" page is part of Settings, with a Nearby Actors table (like Actor Control's) picking the
// actor for "Save Actor Preset". Spells / Combat Styles pages are greyed out when Settings turns them off.
// The Follower drop-down lists only followers in use (Add Follower takes the next free slot). Stats, Combat
// Styles and Settings have a search box at the top that filters their rows. The Settings hotkey opens the
// same pages in a window of their own (tabs), like Actor Control's window.
// Layout helpers are the ones of the user's other SMF mods (Actor Control / Dripping When Aroused SMF).
// Drawing runs on SMF's render thread; values come from LF's snapshot, actions are queued to the main
// thread by LF.

#include "Menu.h"

#include "Files.h"
#include "Hotkey.h"
#include "LF.h"

using namespace ImGuiMCP;

namespace Menu
{
    namespace
    {
        // Every tooltip: a solid background instead of the theme's see-through popup colour.
        template <class... Args>
        void SolidTooltip(const char* a_fmt, Args... a_args)
        {
            ImVec4 bg{ 0.10f, 0.10f, 0.10f, 1.0f };
            if (auto c = GetStyleColorVec4(ImGuiCol_PopupBg)) {
                bg = *c;
            }
            bg.w = 1.0f;
            PushStyleColor(ImGuiCol_PopupBg, bg);
            SetTooltip(a_fmt, a_args...);
            PopStyleColor();
        }

        // ------------------------------------------------------------------
        // Page layout (same as the other mods): blue section headings with a line under them, one row per
        // setting, Reset after it (greyed out at the default). NG 1.1 (user): every row of a SECTION puts its
        // control in one column - 3 spaces right of that section's longest row name - so its bars line up.
        // ------------------------------------------------------------------
        constexpr ImVec4 kSectionBlue{ 0.40f, 0.70f, 1.00f, 1.0f };
        constexpr ImVec4 kValueBlue{ 0.55f, 0.85f, 1.00f, 1.0f };
        constexpr ImVec4 kErrorRed{ 1.00f, 0.45f, 0.45f, 1.0f };
        constexpr float  kMaxControlWidth = 360.0f;
        constexpr float  kMinControlWidth = 120.0f;
        constexpr float  kResetWidth = 60.0f;
        constexpr int    kComboRows = 8;  // searchable drop-downs scroll after this many entries

        std::string                            g_page;     // the page being drawn
        std::string                            g_section;  // the section being drawn (with g_page: its column key)
        bool                                   g_firstSection = true;
        const char*                            g_rowTip = nullptr;
        float                                  g_rowWidth = kMaxControlWidth;
        std::unordered_map<std::string, float> g_nameColumn;  // per page + section: its longest row name (pixels)
        bool                                   g_trackDrag = false;

        // Case-insensitive "contains" for the search boxes.
        bool Contains(std::string_view a_text, const char* a_needle)
        {
            const std::string_view needle(a_needle);
            return needle.empty() || std::search(a_text.begin(), a_text.end(), needle.begin(), needle.end(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            }) != a_text.end();
        }

        // Puts the next item on the same line if a_width still fits there, else on a new line.
        void NextToWidth(float a_width)
        {
            SameLine();
            if (GetContentRegionAvail().x < a_width) {
                NewLine();
            }
        }

        // NextToWidth for a button with this label.
        void NextTo(const char* a_label)
        {
            const auto style = GetStyle();
            NextToWidth(CalcTextSize(a_label, nullptr, true).x + (style ? style->FramePadding.x * 2.0f : 8.0f));
        }

        // Tooltip for the previous item (also while it is greyed out).
        void Tooltip(const char* a_text)
        {
            if (a_text && IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                SolidTooltip("%s", a_text);
            }
        }

        // Clicking the page's scrollbar track jumps there and holding it follows the mouse.
        void ScrollbarJump()
        {
            const float maxY = GetScrollMaxY();
            const auto  style = GetStyle();
            if (maxY <= 0.0f || !style) {
                g_trackDrag = false;
                return;
            }
            const ImVec2 pos = GetWindowPos();
            const ImVec2 size = GetWindowSize();
            const ImVec2 mouse = GetMousePos();
            const float  barW = style->ScrollbarSize;
            const float  top = pos.y + style->WindowBorderSize;
            const float  height = size.y - style->WindowBorderSize * 2.0f;
            const float  grabH = std::clamp(height * size.y / (size.y + maxY), style->GrabMinSize, height);
            const float  grabTop = top + (height - grabH) * (GetScrollY() / maxY);
            if (IsMouseClicked(ImGuiMouseButton_Left) && IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mouse.x >= pos.x + size.x - barW && mouse.x <= pos.x + size.x &&
                mouse.y >= top && mouse.y <= top + height && (mouse.y < grabTop || mouse.y > grabTop + grabH)) {
                g_trackDrag = true;
            }
            if (!IsMouseDown(ImGuiMouseButton_Left)) {
                g_trackDrag = false;
            }
            if (g_trackDrag && height > grabH) {
                const float t = std::clamp((mouse.y - top - grabH * 0.5f) / (height - grabH), 0.0f, 1.0f);
                SetScrollY(t * maxY);
            }
        }

        // NG 1.1 (user): Stats, Combat Styles and Settings have a search box at the top. With text in it only
        // the matching rows are drawn (all rows of a section whose title matches), and a section's heading
        // only when something of it is drawn. Sections made with a_search = false (the pickers) always show.
        std::unordered_map<std::string, std::array<char, 64>> g_pageSearch;  // per page: its search text
        const char*                                            g_filter = "";  // the page's search text ("" = all)
        std::string                                            g_sectionTip;
        bool                                                   g_sectionAll = true;    // every row of the section shows
        bool                                                   g_sectionSearch = true; // the section is filtered
        bool                                                   g_sectionDrawn = true;  // its heading is drawn
        bool                                                   g_anyShown = false;     // something matched the search

        // Top of a page: scrollbar jump; a_page names the page's label column. a_search: the search box.
        void BeginPage(const char* a_page, bool a_search = false)
        {
            ScrollbarJump();
            g_firstSection = true;
            g_page = a_page;
            g_section.clear();
            g_filter = "";
            g_sectionAll = true;
            g_sectionSearch = true;
            g_sectionDrawn = true;
            g_anyShown = false;
            if (a_search) {
                auto&       buf = g_pageSearch[a_page];
                const auto  style = GetStyle();
                const float clear = buf[0] ? CalcTextSize("x").x + (style ? style->FramePadding.x * 2.0f + style->ItemSpacing.x : 12.0f) : 0.0f;
                SetNextItemWidth(std::max(GetContentRegionAvail().x - clear, 60.0f));  // the window's whole width (user)
                InputTextWithHint("##pagesearch", "Search this page...", buf.data(), buf.size());
                if (buf[0]) {
                    SameLine();
                    if (SmallButton("x##clearpage")) {
                        buf[0] = '\0';
                    }
                }
                g_filter = buf.data();
                Spacing();
            }
        }

        // The current section's heading: space above it (not for the first), blue title, a line under it.
        void DrawSection()
        {
            if (!g_firstSection) {
                Dummy(ImVec2{ 0.0f, 12.0f });
            }
            g_firstSection = false;
            g_sectionDrawn = true;
            TextColored(kSectionBlue, "%s", g_section.c_str());
            if (!g_sectionTip.empty() && IsItemHovered()) {
                SolidTooltip("%s", g_sectionTip.c_str());
            }
            Separator();
            Spacing();
        }

        // A section. Its heading is drawn now, or (page search) with its first matching row.
        void Section(const char* a_title, const char* a_tip = nullptr, bool a_search = true)
        {
            g_section = a_title;
            g_sectionTip = a_tip ? a_tip : "";
            g_sectionSearch = a_search;
            g_sectionAll = !a_search || !g_filter[0] || Contains(a_title, g_filter);
            g_sectionDrawn = false;
            if (g_sectionAll) {
                DrawSection();
            }
        }

        // Page search: true when the row / button(s) a_label should be drawn (draws the section heading first).
        bool Show(const char* a_label)
        {
            if (!g_sectionAll && !Contains(a_label, g_filter)) {
                return false;
            }
            if (!g_sectionDrawn) {
                DrawSection();
            }
            if (g_sectionSearch && g_filter[0]) {
                g_anyShown = true;
            }
            return true;
        }

        // Show for a section drawn as one block: when a_keywords match, the rest of the section shows too.
        bool ShowGroup(const char* a_keywords)
        {
            if (!Show(a_keywords)) {
                return false;
            }
            g_sectionAll = true;
            return true;
        }

        // The end of a searched page: a line when nothing matched.
        void NoMatch()
        {
            if (g_filter[0] && !g_anyShown) {
                TextDisabled("Nothing on this page matches \"%s\".", g_filter);
            }
        }

        // Draws a row's name and moves the cursor to the section's control column (3 spaces right of the
        // longest name of the section). Returns false when the column did not fit (the control goes below).
        // a_measure false (NG, user: the gap moved when the long Override row came and went): this name does not
        // widen the column; when longer than the column its control simply follows it.
        bool RowName(const char* a_label, const char* a_tip, float a_minRight, bool a_measure = true)
        {
            const float startX = GetCursorPosX();
            const float rowAvail = GetContentRegionAvail().x;
            float&      column = g_nameColumn[g_page + '/' + g_section];
            const float labelW = CalcTextSize(a_label, nullptr, true).x;
            if (a_measure) {
                column = std::max(column, labelW);
            }
            const float offset = std::max(column, labelW) + CalcTextSize("   ").x;
            AlignTextToFramePadding();
            TextUnformatted(a_label);
            Tooltip(a_tip);
            if (rowAvail - offset >= a_minRight) {
                SameLine(0.0f, 0.0f);
                SetCursorPosX(startX + offset);
                return true;
            }
            return false;
        }

        // "Label: value" on one line, the value (blue) in the section's control column.
        template <class... Args>
        void LabelValue(const char* a_label, const char* a_fmt, Args... a_args)
        {
            if (!Show(a_label)) {
                return;
            }
            RowName(a_label, nullptr, 40.0f);
            TextColored(kValueBlue, a_fmt, a_args...);
        }

        // Starts a setting row: the name, then the cursor at the section's control column. End it with EndRow /
        // EndRowNoReset. Returns false (draw nothing, no End) when the page search hides the row.
        bool BeginRow(const char* a_label, const char* a_tip = nullptr, bool a_measure = true)
        {
            if (!Show(a_label)) {
                return false;
            }
            PushID(a_label);
            constexpr float spacing = 8.0f;
            RowName(a_label, a_tip, kMinControlWidth + spacing + kResetWidth, a_measure);
            g_rowWidth = std::clamp(GetContentRegionAvail().x - spacing - kResetWidth, 60.0f, kMaxControlWidth);
            SetNextItemWidth(g_rowWidth);
            g_rowTip = a_tip;
            return true;
        }

        // Ends a row without a Reset button.
        void EndRowNoReset()
        {
            Tooltip(g_rowTip);
            g_rowTip = nullptr;
            PopID();
        }

        // Ends a row with its Reset button. Returns true when Reset was clicked.
        bool EndRow(bool a_isDefault, const char* a_resetTip = "Back to the default")
        {
            Tooltip(g_rowTip);
            g_rowTip = nullptr;
            NextToWidth(kResetWidth);
            BeginDisabled(a_isDefault);
            const bool reset = Button("Reset", ImVec2{ kResetWidth, 0.0f });
            EndDisabled();
            if (!a_isDefault && IsItemHovered()) {
                SolidTooltip("%s", a_resetTip);
            }
            PopID();
            return reset;
        }

        // Drop-down with a fully opaque list.
        bool BeginSolidCombo(const char* a_id, const char* a_preview, float a_width, ImGuiComboFlags a_flags = ImGuiComboFlags_HeightLarge)
        {
            ImVec4 bg{ 0.10f, 0.10f, 0.10f, 1.0f };
            if (auto c = GetStyleColorVec4(ImGuiCol_PopupBg)) {
                bg = *c;
            }
            bg.w = 1.0f;
            PushStyleColor(ImGuiCol_PopupBg, bg);
            SetNextItemWidth(a_width);
            const bool open = BeginCombo(a_id, a_preview, a_flags);
            PopStyleColor();
            return open;
        }

        // A drop-down with a search box at the top of its list (typing filters, focus is in the box when it
        // opens) and a list that scrolls after kComboRows entries. a_name(i) = entry i's text. Returns the
        // picked index, or -1.
        std::unordered_map<std::string, std::array<char, 64>> g_comboSearch;

        template <class NameOf>
        int SearchCombo(const char* a_id, const char* a_preview, int a_count, int a_current, NameOf a_name)
        {
            int picked = -1;
            if (!BeginSolidCombo(a_id, a_preview, g_rowWidth, ImGuiComboFlags_HeightLargest)) {
                return picked;
            }
            auto&      search = g_comboSearch[std::string(g_page) + '/' + a_id + '/' + std::to_string(GetID(a_id))];
            const bool appearing = IsWindowAppearing();
            if (appearing) {
                search[0] = '\0';
                SetKeyboardFocusHere();
            }
            SetNextItemWidth(-FLT_MIN);
            InputTextWithHint("##search", "Search...", search.data(), search.size());
            std::vector<int> shown;
            for (int i = 0; i < a_count; ++i) {
                if (Contains(a_name(i), search.data())) {
                    shown.push_back(i);
                }
            }
            const auto  style = GetStyle();
            const float pad = style ? style->WindowPadding.y * 2.0f : 8.0f;
            const int   rows = std::clamp(static_cast<int>(shown.size()), 1, kComboRows);
            BeginChild("##list", ImVec2{ 0.0f, GetTextLineHeightWithSpacing() * static_cast<float>(rows) + pad }, ImGuiChildFlags_Border);
            if (shown.empty()) {
                TextDisabled("No match.");
            }
            for (int i : shown) {
                PushID(i);
                if (Selectable(std::string(a_name(i)).c_str(), i == a_current)) {
                    picked = i;
                    CloseCurrentPopup();
                }
                if (i == a_current && appearing) {
                    SetScrollHereY();
                }
                PopID();
            }
            EndChild();
            EndCombo();
            return picked;
        }

        // SearchCombo over Choices; returns the picked form ID or 0. a_none = text when a_current is not listed.
        RE::FormID ChoiceSearchCombo(const char* a_id, const std::vector<LF::Choice>& a_list, RE::FormID a_current, const char* a_none = "(none)")
        {
            int cur = -1;
            for (int i = 0; i < static_cast<int>(a_list.size()); ++i) {
                if (a_list[i].id == a_current) {
                    cur = i;
                    break;
                }
            }
            const int picked = SearchCombo(a_id, cur >= 0 ? a_list[cur].name.c_str() : a_none, static_cast<int>(a_list.size()), cur,
                [&](int i) -> std::string_view { return a_list[i].name; });
            return picked >= 0 ? a_list[picked].id : 0;
        }

        // A dialog: a plain popup (never modal - the page behind is not greyed out) with its title (before
        // "##") as a blue first line. Clicking outside closes it like Cancel. End it with EndPopup().
        bool BeginDialog(const char* a_id)
        {
            ImVec4 bg{ 0.10f, 0.10f, 0.10f, 1.0f };
            if (auto c = GetStyleColorVec4(ImGuiCol_PopupBg)) {
                bg = *c;
            }
            bg.w = 1.0f;
            PushStyleColor(ImGuiCol_PopupBg, bg);
            const bool open = BeginPopup(a_id, ImGuiWindowFlags_AlwaysAutoResize);
            PopStyleColor();
            if (open) {
                const std::string_view title(a_id);
                TextColored(kSectionBlue, "%.*s", static_cast<int>(title.substr(0, title.find("##")).size()), a_id);
                Separator();
            }
            return open;
        }

        // A Yes / No dialog (the MCM's ShowMessage with Yes / No). Open it with OpenPopup(a_id).
        // Returns true in the frame Yes is clicked.
        bool ConfirmDialog(const char* a_id, const std::string& a_message)
        {
            bool yes = false;
            if (BeginDialog(a_id)) {
                TextUnformatted(a_message.c_str());
                Spacing();
                if (Button("Yes", ImVec2{ 80.0f, 0.0f })) {
                    yes = true;
                    CloseCurrentPopup();
                }
                SameLine();
                if (Button("No", ImVec2{ 80.0f, 0.0f })) {
                    CloseCurrentPopup();
                }
                EndPopup();
            }
            return yes;
        }

        // A button that asks first: returns true when the dialog's Yes was clicked.
        bool ConfirmButton(const char* a_label, const char* a_tip, const std::string& a_question, bool a_disabled = false)
        {
            const std::string dlg = std::string(a_label) + "##dlg";
            BeginDisabled(a_disabled);
            if (Button(a_label)) {
                OpenPopup(dlg.c_str());
            }
            EndDisabled();
            Tooltip(a_tip);
            return ConfirmDialog(dlg.c_str(), a_question);
        }

        // A page's Reset All: always its last button (user), a little below the last section, no heading.
        bool ResetAllButton(const char* a_tip, const std::string& a_question, bool a_disabled = false)
        {
            g_sectionAll = !g_filter[0];
            g_sectionSearch = true;
            g_sectionDrawn = true;
            if (!Show("Reset All")) {
                return false;
            }
            Dummy(ImVec2{ 0.0f, 12.0f });
            return ConfirmButton("Reset All", a_tip, a_question, a_disabled);
        }

        // The result line of the last action, blue or red. a_keepRoom (Followers page, user: the rows below jumped when
        // the line came and went after Apply Settings): an empty line of the same height when there is none.
        void DrawNotice(bool a_keepRoom = false)
        {
            bool        error = false;
            std::string text = LF::Notice(error);
            if (!text.empty()) {
                TextColored(error ? kErrorRed : kValueBlue, "%s", text.c_str());
            } else if (a_keepRoom) {
                Dummy(ImVec2{ 0.0f, GetTextLineHeight() });
            }
        }

        // A plain drop-down of Choices (short lists); returns the picked form ID or 0.
        RE::FormID ChoiceCombo(const std::vector<LF::Choice>& a_list, RE::FormID a_current, const char* a_none = "(none)")
        {
            RE::FormID  picked = 0;
            const char* preview = a_none;
            for (auto& c : a_list) {
                if (c.id == a_current) {
                    preview = c.name.c_str();
                    break;
                }
            }
            if (BeginSolidCombo("##v", preview, g_rowWidth)) {
                for (auto& c : a_list) {
                    PushID(static_cast<int>(c.id));
                    if (Selectable(c.name.c_str(), c.id == a_current)) {
                        picked = c.id;
                    }
                    if (c.id == a_current) {
                        SetItemDefaultFocus();
                    }
                    PopID();
                }
                EndCombo();
            }
            return picked;
        }

        // A list table with a header that scrolls after a_maxRows rows. a_width = the column widths' sum.
        // Columns can be dragged wider / narrower (NG 1.1, user).
        bool BeginListTable(const char* a_id, int a_columns, std::size_t a_rows, float a_rowHeight, float a_width, std::size_t a_maxRows = 10)
        {
            ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                    ImGuiTableFlags_SizingFixedFit;
            ImVec2          size{ 0.0f, 0.0f };
            if (a_rows > a_maxRows) {
                const auto  style = GetStyle();
                const float pad = style ? style->CellPadding.y * 2.0f : 4.0f;
                flags |= ImGuiTableFlags_ScrollY;
                // (as Actor Control) as wide as its columns were last frame, so a column dragged wider / narrower
                // resizes the table; while a column border is dragged the whole free width (ImGui only lets a
                // column grow inside the table's own width)
                float width = a_width;
                if (auto* tb = TableFindByID(GetID(a_id)); tb && tb->Columns.Data && tb->ColumnsCount > 0) {
                    const int   last = tb->RightMostEnabledColumn >= 0 ? tb->RightMostEnabledColumn : tb->ColumnsCount - 1;
                    const float measured = tb->Columns.Data[last].MaxX - tb->OuterRect.Min.x;
                    if (measured > 20.0f) {
                        width = measured;
                    }
                    if (tb->ResizedColumn >= 0) {
                        width = std::max(width, GetContentRegionAvail().x - (style ? style->ScrollbarSize : 14.0f) - 2.0f);
                    }
                }
                size = ImVec2{ width + (style ? style->ScrollbarSize : 14.0f) + 2.0f,
                    (GetTextLineHeight() + pad) + (a_rowHeight + pad) * static_cast<float>(a_maxRows) + 2.0f };
            } else {
                flags |= ImGuiTableFlags_NoHostExtendX;
            }
            if (!BeginTable(a_id, a_columns, flags, size)) {
                return false;
            }
            if (a_rows > a_maxRows) {
                TableSetupScrollFreeze(0, 1);
            }
            return true;
        }

        // One table's kept column widths (KeepColumnWidths).
        struct ColumnWidths
        {
            std::vector<float> last;    // the widths last frame
            std::vector<float> stored;  // as the user dragged them (from the settings file at first)
            bool               dirty = false;
            bool               loaded = false;
        };

        // Keeps a table's column widths as the user dragged them (call it after the TableSetupColumn calls, before
        // TableHeadersRow), as Actor Control's KeepColumnWidths: a dragged width is saved in the settings file once
        // the mouse is released (a_save) and given back to the table every frame (ImGui alone forgot it whenever the
        // page was drawn again). NG (user): for the Nearby Actors and the Presets table.
        void KeepColumnWidths(ColumnWidths& a_state, std::vector<float> (*a_load)(), void (*a_save)(const std::vector<float>&))
        {
            if (!a_state.loaded) {
                a_state.stored = a_load();
                a_state.loaded = true;
            }
            auto& s_last = a_state.last;
            auto& s_stored = a_state.stored;
            auto& s_dirty = a_state.dirty;
            auto* table = GetCurrentTable();
            if (!table || !table->Columns.Data || table->ColumnsCount <= 0) {
                return;
            }
            const auto         n = static_cast<std::size_t>(table->ColumnsCount);
            std::vector<float> now(n);
            for (std::size_t i = 0; i < n; ++i) {
                now[i] = table->Columns.Data[i].WidthRequest;
            }
            auto same = [](const std::vector<float>& a_l, const std::vector<float>& a_r) {
                if (a_l.size() != a_r.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < a_l.size(); ++i) {
                    if (std::fabs(a_l[i] - a_r[i]) > 0.5f) {
                        return false;
                    }
                }
                return true;
            };
            if (!s_last.empty() && !same(now, s_last) && std::all_of(now.begin(), now.end(), [](float a_w) { return a_w > 0.0f; })) {
                s_stored = now;  // the user dragged a column
                s_dirty = true;
            } else if (s_stored.size() == n && !same(now, s_stored)) {
                for (std::size_t i = 0; i < n; ++i) {
                    TableSetColumnWidth(static_cast<int>(i), s_stored[i]);
                }
                now = s_stored;
            }
            s_last = now;
            if (s_dirty && !IsMouseDown(ImGuiMouseButton_Left)) {
                s_dirty = false;
                a_save(s_stored);
            }
        }

        // A drag grip right under a list table (call it just after EndTable), as in Actor Control: drag it up /
        // down for fewer / more rows before the table scrolls. a_rows = that row count (1..50); a_save is
        // called with it each time it changes during the drag.
        void ListTableGrip(const char* a_id, int& a_rows, std::size_t a_shown, float a_rowHeight, void (*a_save)(int))
        {
            static int   s_startRows = 0;
            const auto   style = GetStyle();
            const float  step = a_rowHeight + (style ? style->CellPadding.y * 2.0f : 4.0f);
            const ImVec2 tableMin = GetItemRectMin();
            const ImVec2 tableMax = GetItemRectMax();
            const float  width = std::max(tableMax.x - tableMin.x, 60.0f);
            SetCursorScreenPos(ImVec2{ tableMin.x, tableMax.y + 1.0f });
            InvisibleButton(a_id, ImVec2{ width, 8.0f });
            const bool active = IsItemActive();
            const bool hot = active || IsItemHovered();
            if (IsItemActivated()) {
                s_startRows = std::min(a_rows, static_cast<int>(std::max<std::size_t>(a_shown, 1)));
            }
            if (active) {
                // saved as soon as the row count changes (fix, user GIF: it was saved on release, but the page
                // had already reloaded the old saved count in that frame - so the table jumped back)
                const float dy = GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).y;
                const int   rows = std::clamp(s_startRows + static_cast<int>(std::lround(dy / step)), 1, 50);
                if (rows != a_rows) {
                    a_rows = rows;
                    a_save(rows);
                }
            }
            if (hot) {
                SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                if (!active) {
                    SolidTooltip("Drag up / down: fewer / more rows before the list scrolls.");
                }
            }
            // two short lines in the middle show where to grab
            auto*       dl = GetWindowDrawList();
            const ImU32 col = hot ? IM_COL32(66, 150, 250, 255) : IM_COL32(150, 150, 150, 200);
            const float cx = tableMin.x + width * 0.5f;
            const float y = tableMax.y + 3.0f;
            ImDrawListManager::AddLine(dl, ImVec2{ cx - 18.0f, y }, ImVec2{ cx + 18.0f, y }, col, 1.0f);
            ImDrawListManager::AddLine(dl, ImVec2{ cx - 18.0f, y + 3.0f }, ImVec2{ cx + 18.0f, y + 3.0f }, col, 1.0f);
        }

        // The scroll box a folder list is drawn in (as Actor Control's Animation Storage): the folders and the
        // open folder's files together, reaching down to the window's bottom edge (grows with the window);
        // a shorter list only takes its own height (measured the frame before). Close it with EndFolderBox.
        std::unordered_map<std::string, float> g_folderBoxHeight;
        float*                                 g_folderBoxNow = nullptr;

        void BeginFolderBox(const char* a_id)
        {
            const auto  style = GetStyle();
            const float padY = style ? style->WindowPadding.y : 8.0f;
            const float rowH = GetFrameHeightWithSpacing();
            const float windowMax = GetWindowHeight() - padY * 4.0f;
            const float bottom = GetWindowPos().y + GetWindowHeight() - padY;
            const float maxH = std::max(rowH * 4.0f + padY * 2.0f, std::min(bottom - GetCursorScreenPos().y, windowMax));
            g_folderBoxNow = &g_folderBoxHeight[a_id];
            const float h = *g_folderBoxNow > 0.0f ? std::min(*g_folderBoxNow, maxH) : maxH;
            BeginChild(a_id, ImVec2{ 0.0f, h }, ImGuiChildFlags_Border);
        }

        // Ends BeginFolderBox's box and remembers how tall its content was (next frame's height).
        void EndFolderBox()
        {
            if (g_folderBoxNow) {
                const auto  style = GetStyle();
                const float spacing = style ? style->ItemSpacing.y : 4.0f;
                const float padY = style ? style->WindowPadding.y : 8.0f;
                *g_folderBoxNow = GetCursorPosY() - spacing + padY + 2.0f;
                g_folderBoxNow = nullptr;
            }
            EndChild();
        }

        // ---- page open tracking (the MCM's OnConfigOpen) ----
        using Clock = std::chrono::steady_clock;
        std::array<Clock::time_point, 6> g_pageLastDraw{};  // the 5 pages + the Nearby Actors window

        // Call at the top of each page. Returns true when the page was just opened.
        bool NoteDrawn(int a_page)
        {
            const auto now = Clock::now();
            const bool opened = now - g_pageLastDraw[a_page] > 500ms;
            g_pageLastDraw[a_page] = now;
            if (opened) {
                LF::ClearNotice();
            }
            return opened;
        }

        int               g_selected = 0;  // the follower slot all pages work on (the MCM's Nindex)
        Clock::time_point g_selectHold{};  // a just-requested selection (Add Follower) is kept until then

        // Entries whose unapplied choices are dropped (CurrentEdit) - set when Apply moved the follower (take-over / give-back).
        std::array<int, 2> g_forgetPending{ -1, -1 };

        // Pages need LazyFollowers.esp and a loaded game. Returns the snapshot (shared, not copied), or nullptr
        // after drawing why not.
        std::shared_ptr<const LF::Snapshot> Ready()
        {
            if (!LF::FormsFound()) {
                TextColored(kErrorRed, "LazyFollowers.esp was not found (or is not version 0.42.3).");
                Tooltip("See Documents/My Games/Skyrim Special Edition/SKSE/LazyFollowersNG.log for details.");
                return nullptr;
            }
            if (!LF::InGame()) {
                TextDisabled("Load a game first.");
                return nullptr;
            }
            bool moved = false;
            if (const int sel = LF::TakeSelect(&moved); sel >= 0) {
                // NG fix (user: the Override box flipped on / off after Apply Settings): a take-over / give-back moves the
                // follower to another entry; unapplied choices left there from before (e.g. the box unticked for a
                // give-back) showed instead of the new follower's values. Both entries start from what is applied.
                if (moved) {
                    g_forgetPending = { g_selected, sel };
                }
                g_selected = sel;
                g_selectHold = Clock::now() + 1s;  // the snapshot may not list it yet
            }
            g_selected = std::clamp(g_selected, 0, std::max(0, LF::SlotCount() - 1));
            LF::RequestSnapshot(g_selected);
            auto snapPtr = LF::GetSnapshot();
            if (!snapPtr) {
                return nullptr;
            }
            const auto& snap = *snapPtr;
            if (!snap.valid || !snap.lists || snap.slots.empty() || snap.slots.size() != snap.live.size()) {
                TextDisabled("Loading...");
                return nullptr;
            }
            // only followers in the list can be selected: else the first one (stays put when there is none)
            const bool hold = Clock::now() < g_selectHold && g_selected < static_cast<int>(snap.slots.size());
            if (!hold && (g_selected >= static_cast<int>(snap.slots.size()) || !snap.slots[g_selected].Listed())) {
                for (int i = 0; i < static_cast<int>(snap.slots.size()); ++i) {
                    if (snap.slots[i].Listed()) {
                        g_selected = i;
                        break;
                    }
                }
            }
            return snapPtr;
        }

        // A follower's name in the list: its applied name, else "Follower <slot number>" (a real NPC: its name).
        std::string SlotLabel(const LF::Slot& a_s, int a_index)
        {
            return (a_s.Active() || a_s.real) && !a_s.name.empty() ? a_s.name : std::format("Follower {}", a_index + 1);
        }

        // A follower in the Follower drop-down: its name and base NPC form ID (NG 1.1, user); "(NPC)" = a real NPC.
        std::string PickerLabel(const LF::Slot& a_s, int a_index)
        {
            return std::format("{}{}  [{:08X}]", SlotLabel(a_s, a_index), a_s.real ? " (NPC)" : "", a_s.base);
        }

        // The "Follower" drop-down at the top of the follower pages: only followers in use (NG 1.1, user).
        // a_add: with the Add Follower button (Followers page). False = no follower yet (nothing selected).
        bool FollowerPicker(const LF::Snapshot& a_snap, bool a_add)
        {
            const bool any = a_snap.slots[g_selected].Listed();
            BeginRow("Follower", "The follower these pages work on (name + base NPC form ID).\nAdd Follower (Followers page) adds one; Remove takes it off the list.\n"
                                 "Opening the Followers page while looking at a follower selects it.");
            if (!any) {
                TextDisabled(a_add ? "No followers yet - press Add Follower." : "No followers yet - add one on the Followers page.");
            } else {
                std::vector<int>         listed;
                std::vector<std::string> names;
                int                      cur = -1;
                for (int i = 0; i < static_cast<int>(a_snap.slots.size()); ++i) {
                    if (a_snap.slots[i].Listed()) {
                        if (i == g_selected) {
                            cur = static_cast<int>(listed.size());
                        }
                        listed.push_back(i);
                        names.push_back(PickerLabel(a_snap.slots[i], i));
                    }
                }
                const auto preview = PickerLabel(a_snap.slots[g_selected], g_selected);
                if (const int p = SearchCombo("##v", preview.c_str(), static_cast<int>(listed.size()), cur,
                        [&](int k) -> std::string_view { return names[k]; });
                    p >= 0) {
                    g_selected = listed[p];
                }
            }
            EndRowNoReset();
            if (a_add) {
                const bool full = std::all_of(a_snap.slots.begin(), a_snap.slots.end(), [](const LF::Slot& a_s) { return a_s.real || a_s.Listed(); });
                NextTo("Add Follower");
                BeginDisabled(full);
                if (Button("Add Follower")) {
                    LF::AddFollower();
                }
                EndDisabled();
                Tooltip(full ? "All follower slots are in use." : "Adds a new follower (the next free slot), called Follower <number> until you apply a name.");
            }
            return any;
        }

        // Which of the 10 Lazy Followers combat styles a_style is (-1 = another style).
        int StyleIndexOf(RE::FormID a_style)
        {
            for (int i = 0; i < LF::kStyles; ++i) {
                if (auto f = LF::GetStyleForm(i); f && f->GetFormID() == a_style) {
                    return i;
                }
            }
            return -1;
        }

        // ------------------------------------------------------------------
        // Followers page (MCM page " Main")
        // ------------------------------------------------------------------

        // The Appearance section's choices before Apply Settings.
        struct Edit
        {
            std::array<char, 128> name{};
            std::string           presetFile;
            RE::FormID            race = 0;
            RE::FormID            voice = 0;
            bool                  takeOver = false;  // NG 1.2: "Override the modded follower voiced .esp" (off = voice only; a taken-over NPC starts ticked)
            std::string           erinPreset;        // NG (user): Erin race extra preset "<file>.ini" ("" = none)
        };

        // Unapplied choices stay until applied (NG 1.1 fix, user): per follower once something was changed, so
        // switching follower or page no longer puts the old / default values back. Cleared by Reset /
        // Dismiss, Load NPC Preset and when another game is loaded.
        std::unordered_map<int, Edit> g_pending;
        unsigned                      g_pendingLoads = 0;

        // "Jenny.jslot" -> "Jenny" (the follower name a preset gives).
        std::string PresetStem(const std::string& a_file)
        {
            constexpr std::string_view ext = ".jslot";
            return a_file.size() > ext.size() && a_file.ends_with(ext) ? a_file.substr(0, a_file.size() - ext.size()) : a_file;
        }

        // NG (user): the Erin preset with a RaceMenu preset's name ("Ciel.jslot" -> "Ciel.ini"), "" = none there.
        std::string ErinFor(const LF::Lists& a_lists, const std::string& a_presetFile)
        {
            const auto want = PresetStem(a_presetFile) + ".ini";
            for (const auto& f : a_lists.erinPresets) {
                if (_stricmp(f.c_str(), want.c_str()) == 0) {
                    return f;
                }
            }
            return {};
        }

        // The slot's applied values (a new follower: the first preset's name, race and the esp's voice) as an Edit.
        Edit FromSlot(const LF::Snapshot& a_snap, int a_slot)
        {
            const auto& s = a_snap.slots[a_slot];
            Edit        e;
            const auto  name = SlotLabel(s, a_slot);
            strncpy_s(e.name.data(), e.name.size(), name.c_str(), _TRUNCATE);
            // "<name>" -> "<name>.jslot" (the MCM's getPresetIndex), else the first preset
            if (!s.preset.empty()) {
                e.presetFile = s.preset + ".jslot";
            }
            const auto& presets = a_snap.lists->presets;
            if (std::find(presets.begin(), presets.end(), e.presetFile) == presets.end()) {
                e.presetFile = presets.empty() ? std::string{} : presets.front();
            }
            e.race = s.race;
            // NG (user): a follower without a preset yet starts with the race of the preset shown
            if (s.preset.empty()) {
                if (auto it = a_snap.lists->presetRaces.find(e.presetFile); it != a_snap.lists->presetRaces.end()) {
                    e.race = it->second;
                }
                // NG (user): ... and the name of that preset (Jenny.jslot -> "Jenny")
                if (!e.presetFile.empty()) {
                    strncpy_s(e.name.data(), e.name.size(), PresetStem(e.presetFile).c_str(), _TRUNCATE);
                }
                e.erinPreset = ErinFor(*a_snap.lists, e.presetFile);  // NG (user): ... and its Erin preset (same name)
            } else {
                e.erinPreset = s.erinPreset;
            }
            e.voice = s.voice;
            e.takeOver = s.real;  // NG 1.2: a real NPC (e.g. taken-over Sofia) is the override - unticking undoes it
            return e;
        }

        // What the Appearance section shows for the selected slot. Change it, then call Touched().
        Edit& CurrentEdit(const LF::Snapshot& a_snap)
        {
            static Edit view;
            if (const auto loads = LF::GameLoads(); loads != g_pendingLoads) {
                g_pending.clear();
                g_pendingLoads = loads;
            }
            for (auto& f : g_forgetPending) {
                if (f >= 0) {
                    g_pending.erase(f);
                    f = -1;
                }
            }
            if (auto it = g_pending.find(g_selected); it != g_pending.end()) {
                return it->second;
            }
            view = FromSlot(a_snap, g_selected);
            return view;
        }

        // The user changed something in the Appearance section.
        void Touched(const Edit& a_e)
        {
            const Edit copy = a_e;
            g_pending[g_selected] = copy;
        }

        std::string g_npcPreset;          // NPC Preset drop-down
        int         g_npcPresetFor = -1;  // the slot it was filled for

        void __stdcall RenderFollowers()
        {
            const bool opened = NoteDrawn(0);
            if (opened && LF::FormsFound() && LF::InGame()) {
                LF::OnFollowersPageOpened();
            }
            BeginPage("followers");
            auto snapOpt = Ready();
            if (!snapOpt) {
                return;
            }
            const auto& snap = *snapOpt;
            const auto& lists = *snap.lists;

            Section("Follower", nullptr, false);
            if (!FollowerPicker(snap, true)) {
                DrawNotice();
                return;
            }
            const auto&       s = snap.slots[g_selected];
            const auto&       live = snap.live[g_selected];
            const std::string who = SlotLabel(s, g_selected);
            // NG 1.1 (user): Distance + Location instead of Status
            if (!live.enabled && s.real) {
                LabelValue("Distance:", "%s", "Disabled");
                Tooltip("The NPC is disabled (by its own mod or the game) or not in the game right now.");
                LabelValue("Location:", "%s", "Unknown");  // NG fix: same rows as when enabled (it is disabled for a moment while its look changes)
            } else if (!live.enabled) {
                LabelValue("Distance:", "%s", "In storage");
                Tooltip("Dismissed or never summoned (disabled in Lazy Followers' storage cell).");
                LabelValue("Location:", "%s", "Lazy Followers storage");
            } else {
                if (snap.selDistance >= 0.0f) {
                    LabelValue("Distance:", "%.1f m", snap.selDistance);
                } else {
                    LabelValue("Distance:", "%s", "Not near you");
                    Tooltip("In another cell or worldspace.");
                }
                LabelValue("Location:", "%s", snap.selLocation.empty() ? "Unknown" : snap.selLocation.c_str());
            }
            // NG (user): Keep in New Game per follower (was one Settings option, removed in (64))
            if (BeginRow("Keep in New Game", s.real ? "Ticked: every save writes this NPC (name, preset, race, worn gear) to\n"
                                                      "Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGKeep.json, and a NEW game takes it over again\n"
                                                      "(like Apply Settings with Override the modded follower voiced .esp) half a second after you close\n"
                                                      "RaceMenu (character creation). Pre Summon (shown when ticked) decides whether it comes to you."
                                                    : "Ticked: every save writes this follower (name, preset, race, voice, sex, style, class, spells, worn gear)\n"
                                                      "to Data/SKSE/Plugins/LazyFollowers/LazyFollowersNGKeep.json, and a NEW game brings it back\n"
                                                      "half a second after you close RaceMenu (character creation). Pre Summon (shown when ticked) decides whether it comes to you.")) {
                bool v = s.keep;
                bool changed = Checkbox("##v", &v);
                if (EndRow(!v)) {
                    v = false;
                    changed = true;
                }
                if (changed) {
                    LF::SetKeep(g_selected, v);
                }
            }
            // NG (user): only with Keep in New Game ticked
            if (s.keep && BeginRow("Pre Summon", "Ticked: in the new game this follower is summoned to you once it is back.\n"
                                                  "Unticked: it comes back but stays where it is (Summon NPC / Summon All bring it).")) {
                bool v = s.preSummon;
                bool changed = Checkbox("##v", &v);
                if (EndRow(!v)) {
                    v = false;
                    changed = true;
                }
                if (changed) {
                    LF::SetPreSummon(g_selected, v);
                }
            }
            DrawNotice(true);

            Section("Appearance", "Pick name, preset, race and voice, then Apply Settings (the MCM's \"Apply settings\").\n"
                                  "Your choices stay while you look at other followers or pages until you apply them.");
            auto& e = CurrentEdit(snap);
            BeginRow("Name", s.real ? "The NPC's name in game (nameplate, talk menu, subtitles) - applied with Apply Settings.\nIts recorded voice still says its own name; Remove gives it back."
                                    : "The follower's name (applied with Apply Settings).");
            if (InputText("##v", e.name.data(), e.name.size())) {
                Touched(e);
            }
            EndRowNoReset();
            BeginRow("Preset", "RaceMenu presets in Data/SKSE/Plugins/CharGen/Exported (.jslot) - make new ones with\n"
                               "Settings > Save Actor Preset. The follower becomes male or female to match the preset.\n"
                               "A .nif with the preset's name in that folder is used as the head mesh (as in the old mod).");
            if (lists.presets.empty()) {
                TextDisabled("No presets in CharGen/Exported");
            } else {
                int cur = -1;
                for (int i = 0; i < static_cast<int>(lists.presets.size()); ++i) {
                    if (lists.presets[i] == e.presetFile) {
                        cur = i;
                    }
                }
                if (const int p = SearchCombo("##v", e.presetFile.c_str(), static_cast<int>(lists.presets.size()), cur,
                        [&](int i) -> std::string_view { return lists.presets[i]; });
                    p >= 0) {
                    e.presetFile = lists.presets[p];
                    // NG (user): picking a preset also puts its name in Name (Jenny.jslot -> "Jenny"; can still be typed over)
                    strncpy_s(e.name.data(), e.name.size(), PresetStem(e.presetFile).c_str(), _TRUNCATE);
                    // NG (user): picking a preset also picks the race it was made for (Race can still be changed)
                    if (auto it = lists.presetRaces.find(e.presetFile); it != lists.presetRaces.end()) {
                        e.race = it->second;
                    }
                    // NG (user): ... and the Erin preset with its name, when there is one (else Erin Preset stays as it is)
                    if (auto erin = ErinFor(lists, e.presetFile); !erin.empty()) {
                        e.erinPreset = erin;
                    }
                    Touched(e);
                }
            }
            EndRowNoReset();
            BeginRow("Race", "Every playable race of your load order (rebuilt each time this page opens),\nplus races of NPCs you looked at when opening the page.\n"
                             "The preset's own race (found from its head parts) is listed first and picked when you pick a preset.");
            {
                // NG (user): the preset's race first in the list, marked
                std::vector<LF::Choice> races = lists.races;
                if (auto it = lists.presetRaces.find(e.presetFile); it != lists.presetRaces.end()) {
                    if (auto r = std::find_if(races.begin(), races.end(), [&](const LF::Choice& c) { return c.id == it->second; }); r != races.end()) {
                        LF::Choice first = *r;
                        first.name += "  (preset's race)";
                        races.erase(r);
                        races.insert(races.begin(), std::move(first));
                    }
                }
                if (auto id = ChoiceSearchCombo("##v", races, e.race)) {
                    e.race = id;
                    Touched(e);
                }
            }
            EndRowNoReset();
            const bool erinRace = lists.erinRaces.contains(e.race);  // NG (user): Erin Preset only for an Erin race
            // NG (user): the Erin race's own preset (extra tints, eye colour set, skin, head-part adjust) - RaceMenu's .jslot
            // does not have it. Only while an Erin race is picked in Race (MuErinRace.dll installed).
            if (!lists.erinPresets.empty() && erinRace) {
                BeginRow("Erin Preset", "The Erin race's own preset (Data/SKSE/Plugins/ErinRace/Preset, .ini): extra tints, eye colour set,\n"
                                        "skin and head-part adjustments - things RaceMenu's .jslot does not keep.\n"
                                        "Loaded onto the follower after the RaceMenu preset (Apply Settings, and each time its look is applied).\n"
                                        "Picking a preset picks the Erin preset with the same name, when there is one.\n"
                                        "Shown only while an Erin race is picked. Remove takes it off again.");
                int cur = 0;  // 0 = (none)
                for (int i = 0; i < static_cast<int>(lists.erinPresets.size()); ++i) {
                    if (_stricmp(lists.erinPresets[i].c_str(), e.erinPreset.c_str()) == 0) {
                        cur = i + 1;
                    }
                }
                const auto shown = cur ? lists.erinPresets[cur - 1] : std::string("(none)");
                if (const int p = SearchCombo("##erin", shown.c_str(), static_cast<int>(lists.erinPresets.size()) + 1, cur,
                        [&](int i) -> std::string_view { return i ? std::string_view(lists.erinPresets[i - 1]) : std::string_view("(none)"); });
                    p >= 0) {
                    e.erinPreset = p ? lists.erinPresets[p - 1] : std::string{};
                    Touched(e);
                }
                EndRowNoReset();
            }
            // two lists like the MCM's; the follower gets the voice picked last (the other shows "(none)")
            BeginRow("Female Voice", "Starred voices are vanilla follower-friendly.\nVoices of NPCs you looked at when opening the page are added.\nThe follower gets the voice you picked last (female, male or other list).");
            if (auto id = ChoiceSearchCombo("##v", lists.femaleVoices, e.voice)) {
                e.voice = id;
                Touched(e);
            }
            EndRowNoReset();
            BeginRow("Male Voice", "Starred voices are vanilla follower-friendly.\nVoices of NPCs you looked at when opening the page are added.\nThe follower gets the voice you picked last (female, male or other list).");
            if (auto id = ChoiceSearchCombo("##v", lists.maleVoices, e.voice)) {
                e.voice = id;
                Touched(e);
            }
            EndRowNoReset();
            BeginRow("Other Voice", "Every other voice type of your load order (modded voices, creature / unique voices...).\n"
                                    "Names need po3's Tweaks (editor IDs); without it they show the form ID. Not every voice has follower dialogue.\n"
                                    "The follower gets the voice you picked last (female, male or other list).\n"
                                    "A modded voice (e.g. Sofia's): the follower uses its mod's lines; see Override the modded follower voiced .esp\n"
                                    "(shown when such a voice is picked).");
            if (auto id = ChoiceSearchCombo("##v", lists.otherVoices, e.voice)) {
                e.voice = id;
                Touched(e);
            }
            EndRowNoReset();
            // NG 1.2 (user, option 3): taking over a modded voice's own NPC is a choice per follower, off by default.
            // NG 1.2 (user): renamed from "Take Over NPC"; also shown on the taken-over NPC (ticked) so it can be undone.
            // NG (user): renamed "... voiced .esp"; on a slot it only shows when the picked voice belongs to a modded
            // follower NPC of the game (Lists::ownedVoices, e.g. Sofia's), so it doesn't confuse with other voices
            const bool showOverride = s.real ? s.moddedVoice : lists.ownedVoices.contains(e.voice);
            // NG (user): the voice's NPC is already another follower's Override -> Apply Settings with the box ticked asks
            // to move it here (Yes: that follower gives the NPC back - it goes to a free slot with the voice - and this
            // follower takes it over with this page's settings). Ticking the box alone does nothing yet (user). Also on a
            // taken-over NPC's page when another follower's voice is picked (it is given back, see LF::Apply).
            int holder = -1;
            if (showOverride && e.voice) {
                for (int k = 0; k < static_cast<int>(snap.slots.size()); ++k) {
                    const auto& o = snap.slots[k];
                    if (k != g_selected && o.real && o.Active() && o.voice == e.voice) {
                        holder = k;
                        break;
                    }
                }
            }
            bool openMove = false;
            if (showOverride) {
                BeginRow("Override the modded follower voiced .esp",
                    s.real ? "Ticked: this NPC (the voice's own NPC) is your follower with this preset, race and name.\n"
                             "Untick, then Apply Settings: it is given back (own look, name and outfit, sent to its own place)\n"
                             "and a free Lazy Followers slot becomes the follower with this preset, race and name and the\n"
                             "NPC's voice (voice only - its mod's lines are swapped in). The follower's gear goes along.\n"
                             "Another modded follower's voice picked (box ticked) + Apply Settings: this NPC is given back and\n"
                             "that voice's own NPC becomes the follower with this preset, race and name (gear and Keep boxes go along)."
                           : "Off: the follower only borrows the voice - its mod's lines are swapped in; the voice's own NPC\n"
                             "(e.g. Sofia) stays as it is. Several followers can share one voice.\n"
                             "On: Apply Settings makes the voice's own NPC the follower with this preset, race and name -\n"
                             "its own voice, dialogue, quests and scripts (its look changes while it is your follower);\n"
                             "this slot goes back to storage. One follower per voice; untick it there (or Remove) to give the NPC back.\n"
                             "Shown only for a modded follower's voice whose own NPC is in your game.",
                    false);  // its long name does not widen the section's column (no gap jump when it shows / hides)
                bool v = e.takeOver;
                if (Checkbox("##v", &v)) {  // NG (user): ticking only ticks - Apply Settings does (and asks) the rest
                    e.takeOver = v;
                    Touched(e);
                }
                if (EndRow(e.takeOver == s.real)) {
                    e.takeOver = s.real;
                    Touched(e);
                }
            }
            if (ConfirmButton("Apply Settings", s.real ? "Applies name, preset, race and voice to the NPC (full preset apply)\nand summons it to you (like Summon NPC)."
                                                       : "Applies name, preset, race and voice to the follower (full preset apply)\nand summons it to you (like Summon NPC).\n"
                                                         "With Override the modded follower voiced .esp on and a modded voice whose own NPC is in your game (e.g. Sofia): that NPC\n"
                                                         "becomes the follower with this preset, race and name (its own voice and dialogue); this slot goes back to storage.",
                    std::format("Apply settings to {}?", who), e.presetFile.empty())) {
                Touched(e);  // what was applied stays shown
                if (e.takeOver && holder >= 0) {
                    openMove = true;
                } else {
                    LF::ApplyRequest req{ e.name.data(), e.race, e.voice, e.presetFile, s.real ? e.takeOver : e.takeOver && showOverride };  // a slot's hidden box counts as off
                    req.erinPreset = erinRace ? e.erinPreset : std::string{};  // another race: no Erin preset (the old one is taken off)
                    LF::Apply(g_selected, std::move(req));
                }
            }
            if (openMove) {
                OpenPopup("Override already in use##moveOverride");
            }
            if (holder >= 0) {
                const auto& h = snap.slots[holder];
                if (ConfirmDialog("Override already in use##moveOverride",
                        std::format("{} is already the Override of your follower {}.\n"
                                    "Remove it from {} and put it on {}?\n\n"
                                    "{} gives {} back and goes to a free slot with that voice (voice only);\n"
                                    "{} takes {} over now with this page's name, preset and race.",
                            h.defaultName, h.name, h.name, who, h.name, h.defaultName, who, h.defaultName))) {
                    e.takeOver = true;
                    Touched(e);
                    LF::ApplyRequest req{ e.name.data(), e.race, e.voice, e.presetFile, true };  // no preset: Apply says so
                    req.erinPreset = erinRace ? e.erinPreset : std::string{};
                    LF::MoveOverride(holder, g_selected, std::move(req));
                }
            }

            if (!lists.npcPresets.empty() && !s.real) {
                Section("NPC Preset", "Followers saved with \"Save Each NPC Separately\" (Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs).");
                if (g_npcPresetFor != g_selected || std::find(lists.npcPresets.begin(), lists.npcPresets.end(), g_npcPreset) == lists.npcPresets.end()) {
                    const auto mine = who + ".json";
                    g_npcPreset = std::find(lists.npcPresets.begin(), lists.npcPresets.end(), mine) != lists.npcPresets.end() ? mine : lists.npcPresets.front();
                    g_npcPresetFor = g_selected;
                }
                BeginRow("NPC Preset", "Saved follower: name, preset, race, voice, style, class, spells and gear.");
                if (BeginSolidCombo("##v", g_npcPreset.c_str(), g_rowWidth)) {
                    for (auto& p : lists.npcPresets) {
                        if (Selectable(p.c_str(), p == g_npcPreset)) {
                            g_npcPreset = p;
                        }
                    }
                    EndCombo();
                }
                EndRowNoReset();
                if (ConfirmButton("Load NPC Preset", "Applies the saved follower to this slot and summons it (the slot becomes male or female as saved).",
                        std::format("Apply preset {} to {}?", g_npcPreset, who), g_npcPreset.empty())) {
                    g_pending.erase(g_selected);
                    LF::LoadNPCPreset(g_selected, g_npcPreset);
                }
            }

            Section("Actions");
            if (ConfirmButton("Summon NPC", "Teleports the follower to you.", std::format("Summon {}?", who))) {
                LF::Summon(g_selected);
            }
            if (s.Active()) {
                NextTo("Teleport to NPC");
                if (ConfirmButton("Teleport to NPC", "Teleports you to the follower.", std::format("Teleport to {}?", who))) {
                    LF::TeleportTo(g_selected);
                }
            }
            NextTo("Remove");
            if (s.real ? ConfirmButton("Remove", "Gives the NPC its own look and name back and takes it off the Follower list;\nit goes back to its own place in the world (its own mod may still call it a follower).",
                             std::format("Remove {}? (own look and name back, sent to its own place)", who))
                       : ConfirmButton("Remove", "Resets all changes to the follower, sends it back to storage and takes it off the Follower list.",
                             std::format("Remove {}? (all changes reset, back to storage)", who))) {
                g_pending.erase(g_selected);
                LF::Dismiss(g_selected);
            }
            if (live.loaded) {
                NextTo("Copy Kit");
                if (ConfirmButton("Copy Kit", "Copies your whole inventory to the follower (not gold or quest items; what it already has is not doubled).\n"
                                                  "Include Equipment: also what you wear. Auto Equip: puts that on the follower.", std::format("Copy your inventory to {}?", who))) {
                    LF::CopyKit(g_selected);
                }
            }
            BeginRow("Auto Equip", "Copy Kit also puts on the armor and weapons you wear. WARNING - can crash the game (as in the original mod).");
            {
                bool v = LF::AutoEquip();
                if (Checkbox("##v", &v)) {
                    LF::SetAutoEquip(v);
                }
                if (EndRow(!v)) {
                    LF::SetAutoEquip(false);
                }
            }
            BeginRow("Include Equipment", "Copy Kit also copies the armor and weapons you are wearing (on by default).\n"
                                          "Off: only the rest of your inventory is copied, and Auto Equip has nothing to put on.");
            {
                bool v = LF::IncludeEquipment();
                if (Checkbox("##v", &v)) {
                    LF::SetIncludeEquipment(v);
                }
                if (EndRow(v)) {
                    LF::SetIncludeEquipment(true);
                }
            }

            Section("All Followers");
            if (ConfirmButton("Apply to All", "Does a FULL preset reload on all followers.",
                    "Force full update of all followers?\nWarning - will reapply / override all tints, overlays etc. from their presets.")) {
                LF::RefreshAll();
            }
            NextTo("Summon All");
            if (ConfirmButton("Summon All", "Summons every follower that is not following you right now.", "Summon all unassigned followers?")) {
                LF::SummonAll();
            }
            if (snap.fgt) {
                NextTo("Followers Travel");
                if (ConfirmButton("Followers Travel", "Followers not following you are handed to Follower Goes on a Trip (max 24 in total).",
                        "Summon all unassigned followers and send them on a trip?")) {
                    LF::SendTravel();
                }
            }

            // NG (user): also for a taken-over NPC (its own style / class come back on Remove)
            Section("Behaviour", s.real ? "Combat style and class are applied right away.\nRemove (or giving it back) puts the NPC's own ones back."
                                        : "Combat style and class are applied right away.");
            BeginRow("Combat Style", "One of the 10 Lazy Followers combat styles (edit them on the Combat Styles page).");
            if (!LF::StylesEnabled()) {
                TextDisabled("Disabled (Settings)");
            } else {
                const int cur = StyleIndexOf(s.style);
                if (const int p = SearchCombo("##v", cur >= 0 ? snap.styleNames[cur].c_str() : "(not a Lazy Followers style)", LF::kStyles, cur,
                        [&](int i) -> std::string_view { return snap.styleNames[i]; });
                    p >= 0) {
                    LF::SetStyle(g_selected, p);
                }
            }
            EndRowNoReset();
            BeginRow("Class", "The follower's class (skill growth).");
            if (!LF::ClassesEnabled()) {
                TextDisabled("Disabled (Settings)");
            } else if (auto id = ChoiceSearchCombo("##v", lists.classes, s.npcClass)) {
                LF::SetClass(g_selected, id);
            }
            EndRowNoReset();
        }

        // ------------------------------------------------------------------
        // Stats page (MCM page " Stats")
        // ------------------------------------------------------------------
        constexpr std::array<const char*, LF::kStatCount> kStatNames{ "WeaponSpeedMult", "BowSpeedBonus", "AttackDamageMult", "SpeedMult", "DamageResist",
            "MagicResist", "Health", "Stamina", "Magicka", "HealRate", "StaminaRate", "MagickaRate" };
        constexpr std::array<const char*, LF::kStatCount> kStatLabels{ "Weapon Speed", "Bow Speed", "Attack Damage Mult", "Movement Speed", "Damage Resist",
            "Magic Resist", "Health", "Stamina", "Magicka", "Heal Rate", "Stamina Rate", "Magicka Rate" };
        constexpr std::array<const char*, LF::kSkillCount> kSkillNames{ "OneHanded", "TwoHanded", "Marksman", "Block", "Smithing", "HeavyArmor", "LightArmor",
            "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speechcraft", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting" };
        constexpr std::array<const char*, LF::kSkillCount> kSkillLabels{ "One-Handed", "Two-Handed", "Archery", "Block", "Smithing", "Heavy Armor", "Light Armor",
            "Pickpocket", "Lockpicking", "Sneak", "Alchemy", "Speech", "Alteration", "Conjuration", "Destruction", "Illusion", "Restoration", "Enchanting" };

        constexpr const char* kStatResetTip = "Back to the value it had before you changed it";

        void __stdcall RenderStats()
        {
            NoteDrawn(1);
            BeginPage("stats", true);
            auto snapOpt = Ready();
            if (!snapOpt) {
                return;
            }
            const auto& snap = *snapOpt;
            Section("Follower", nullptr, false);
            if (!FollowerPicker(snap, false)) {
                return;
            }
            DrawNotice();
            const auto& s = snap.slots[g_selected];

            // NG 1.1 (user): Reset per row = the value before the first change (kept in the save)
            Section("Stats", "Type a value and press Enter (Papyrus ForceActorValue, like the MCM).\nReset = the value it had before you changed it.");
            for (int i = 0; i < LF::kStatCount; ++i) {
                // AttackDamageMult was a 0-2 slider in the MCM
                if (!BeginRow(kStatLabels[i], i == 2 ? "Attack damage multiplier (0 - 2)." : kStatNames[i])) {
                    continue;
                }
                float v = snap.stats[i];
                if (i == 2) {
                    SliderFloat("##v", &v, 0.0f, 2.0f, "%.2f");
                    if (IsItemDeactivatedAfterEdit()) {
                        LF::SetStat(g_selected, kStatNames[i], v);
                    }
                } else if (InputFloat("##v", &v, 0.0f, 0.0f, "%.2f", ImGuiInputTextFlags_EnterReturnsTrue)) {
                    LF::SetStat(g_selected, kStatNames[i], v);
                }
                if (EndRow(!s.statDefaults.contains(kStatNames[i]), kStatResetTip)) {
                    LF::ResetStat(g_selected, kStatNames[i]);
                }
            }

            Section("Skills", "Drag and release to set (0 - 100).\nReset = the value it had before you changed it.");
            for (int i = 0; i < LF::kSkillCount; ++i) {
                if (!BeginRow(kSkillLabels[i])) {
                    continue;
                }
                int v = static_cast<int>(snap.skills[i] + 0.5f);
                SliderInt("##v", &v, 0, 100);
                if (IsItemDeactivatedAfterEdit()) {
                    LF::SetStat(g_selected, kSkillNames[i], static_cast<float>(v));
                }
                if (EndRow(!s.statDefaults.contains(kSkillNames[i]), kStatResetTip)) {
                    LF::ResetStat(g_selected, kSkillNames[i]);
                }
            }

            if (ResetAllButton("Every stat and skill you changed back to the value it had before.",
                    std::format("Reset all stats and skills of {}?", SlotLabel(s, g_selected)), s.statDefaults.empty())) {
                LF::ResetAllStats(g_selected);
            }
            NoMatch();
        }

        // ------------------------------------------------------------------
        // Spells page (MCM page " Spells")
        // ------------------------------------------------------------------
        std::array<char, 64>                         g_spellSearch{};    // all folders
        std::unordered_map<int, std::array<char, 64>> g_schoolSearch;    // per folder ("Search these files...")
        int                                          g_spellFolder = -1;  // the open folder (-1 = none)
        bool                                         g_spellScrollTop = false;

        // A page turned off in Settings: one line why; the caller greys the rest out (BeginDisabled).
        void DisabledNote(const char* a_setting)
        {
            TextColored(kErrorRed, "Turned off in Settings > %s.", a_setting);
        }

        void __stdcall RenderSpells()
        {
            NoteDrawn(2);
            BeginPage("spells");
            auto snapOpt = Ready();
            if (!snapOpt) {
                return;
            }
            const auto& snap = *snapOpt;
            const bool  off = !LF::SpellsEnabled();
            if (off) {
                DisabledNote("Custom Spells");
            }
            BeginDisabled(off);
            Section("Follower", nullptr, false);
            if (!FollowerPicker(snap, false)) {
                EndDisabled();
                return;
            }
            DrawNotice();
            const auto& s = snap.slots[g_selected];
            const auto& spells = snap.lists->spells;
            auto        nameOf = [&](RE::FormID a_id) -> std::string {
                for (auto& c : spells) {
                    if (c.id == a_id) {
                        return c.name;
                    }
                }
                auto f = RE::TESForm::LookupByID(a_id);
                return f ? f->GetName() : std::format("{:08X}", a_id);
            };

            if (!s.spells.empty()) {
                Section("Added Spells", "Right-click a spell to remove it from the follower.");
                RE::FormID remove = 0;
                if (BeginListTable("##added", 1, s.spells.size(), GetTextLineHeight(), 320.0f)) {
                    TableSetupColumn("Spell", ImGuiTableColumnFlags_WidthFixed, 320.0f);
                    TableHeadersRow();
                    for (auto id : s.spells) {
                        PushID(static_cast<int>(id));
                        TableNextRow();
                        TableSetColumnIndex(0);
                        Selectable(nameOf(id).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                        if (BeginPopupContextItem("##rowmenu")) {
                            if (Selectable("Remove spell")) {
                                remove = id;
                            }
                            EndPopup();
                        }
                        PopID();
                    }
                    EndTable();
                }
                if (remove) {
                    LF::ToggleSpell(g_selected, remove);
                }
            }

            // NG 1.1 (user): folders like Actor Control's Animation Storage - one scroll box, a folder per magic
            // school, "Search these files..." inside the open folder; an open folder hides the others (only
            // with an empty search box - a search shows every folder with a match).
            Section("Spells", "Click a spell to add it to the follower (click again to remove). (L) / (R) = hand, (-) = either.\n"
                              "Folders = magic school of the spell's strongest effect.");
            SetNextItemWidth(320.0f);
            InputTextWithHint("##spellsearch", "Search folders and spells...", g_spellSearch.data(), g_spellSearch.size());
            if (g_spellSearch[0]) {
                SameLine();
                if (SmallButton("x##clearspells")) {
                    g_spellSearch[0] = '\0';
                }
            }
            NextToWidth(200.0f);
            TextDisabled("%zu spells in %zu folders", spells.size(), LF::kSpellSchools.size());
            if (!LF::AllSpellsLoaded()) {
                NextTo("Load All Spells");
                if (ConfirmButton("Load All Spells", "Adds every playable spell of your load order to this list.\nCan be a long list, and not every spell works on NPCs.",
                        "Scan the load order and add all playable spells?")) {
                    LF::LoadAllSpells();
                }
            }
            const bool searching = g_spellSearch[0] != '\0';
            const int  focus = searching ? -1 : g_spellFolder;
            BeginFolderBox("##spellfolders");
            if (g_spellScrollTop) {
                SetScrollY(0.0f);  // a folder was just opened: its header at the box's top
                g_spellScrollTop = false;
            }
            int shownFolders = 0;
            for (int g = 0; g < static_cast<int>(LF::kSpellSchools.size()); ++g) {
                const bool       folderHit = searching && Contains(LF::kSpellSchools[g], g_spellSearch.data());
                auto&            local = g_schoolSearch[g];
                std::vector<int> rows;
                std::size_t      all = 0;
                for (int k = 0; k < static_cast<int>(spells.size()); ++k) {
                    if (spells[k].group != g) {
                        continue;
                    }
                    ++all;
                    if ((!searching || folderHit || Contains(spells[k].name, g_spellSearch.data())) && Contains(spells[k].name, local.data())) {
                        rows.push_back(k);
                    }
                }
                if (all == 0 || (searching && rows.empty() && !local[0]) || (focus >= 0 && focus != g)) {
                    continue;  // empty folder / no match / another folder is open
                }
                ++shownFolders;
                PushID(g);
                SetNextItemOpen(searching || focus == g, ImGuiCond_Always);
                const auto header = std::format("{}  ({})###school", LF::kSpellSchools[g], searching || local[0] ? rows.size() : all);
                const bool open = CollapsingHeader(header.c_str());
                if (IsItemToggledOpen() && !searching) {
                    g_spellFolder = open ? g : -1;
                    g_spellScrollTop = open;
                }
                if (open) {
                    SetNextItemWidth(320.0f);
                    InputTextWithHint("##local", "Search these files...", local.data(), local.size());
                    if (local[0]) {
                        SameLine();
                        if (SmallButton("x")) {
                            local[0] = '\0';
                        }
                    }
                    if (rows.empty()) {
                        TextDisabled("No match.");
                    }
                    for (int k : rows) {
                        const auto& c = spells[k];
                        PushID(static_cast<int>(c.id));
                        const bool has = std::find(s.spells.begin(), s.spells.end(), c.id) != s.spells.end();
                        if (Selectable(c.name.c_str(), has)) {
                            LF::ToggleSpell(g_selected, c.id);
                        }
                        if (IsItemHovered()) {
                            SolidTooltip(has ? "The follower has it - click to remove" : "Click to add to the follower");
                        }
                        PopID();
                    }
                }
                PopID();
            }
            if (shownFolders == 0) {
                TextDisabled("No match.");
            }
            EndFolderBox();
            EndDisabled();
        }

        // ------------------------------------------------------------------
        // Combat Styles page (MCM page " Combat")
        // ------------------------------------------------------------------
        int                  g_styleIndex = 0;
        std::array<char, 64> g_styleName{};
        int                  g_styleNameFor = -1;
        int                  g_styleFollower = -1;  // the follower whose style was picked for editing

        // One combat style value: slider over the MCM's range, Reset to the esp value.
        void StyleSlider(RE::TESCombatStyle* a_style, std::array<float, LF::kStyleValues>& a_v, const std::array<float, LF::kStyleValues>& a_def, int a_index,
            const char* a_label, float a_max)
        {
            if (!BeginRow(a_label)) {
                return;
            }
            float v = a_v[a_index];
            bool  changed = SliderFloat("##v", &v, 0.0f, a_max, "%.2f");
            if (EndRow(std::abs(a_v[a_index] - a_def[a_index]) < 0.0001f)) {
                v = a_def[a_index];
                changed = true;
            }
            if (changed) {
                a_v[a_index] = v;
                LF::WriteStyle(a_style, a_v);
            }
        }

        void __stdcall RenderStyles()
        {
            NoteDrawn(3);
            BeginPage("styles", true);
            auto snapOpt = Ready();
            if (!snapOpt) {
                return;
            }
            const auto& snap = *snapOpt;
            const bool  off = !LF::StylesEnabled();
            if (off) {
                DisabledNote("Custom Combat Styles");
            }
            BeginDisabled(off);
            struct EndOff
            {
                ~EndOff() { EndDisabled(); }
            } endOff;  // every return below ends the greyed-out block

            // NG 1.1 (user): the Follower section as on the Spells page; picking a follower opens its style
            Section("Follower", nullptr, false);
            if (FollowerPicker(snap, false)) {
                const int uses = StyleIndexOf(snap.slots[g_selected].style);
                if (g_styleFollower != g_selected) {
                    g_styleFollower = g_selected;
                    if (uses >= 0) {
                        g_styleIndex = uses;
                    }
                }
                LabelValue("Uses:", "%s", uses >= 0 ? snap.styleNames[uses].c_str() : "(not a Lazy Followers style)");
                Tooltip("The combat style this follower uses (change it on the Followers page > Behaviour).");
            }
            DrawNotice();

            g_styleIndex = std::clamp(g_styleIndex, 0, LF::kStyles - 1);
            auto style = LF::GetStyleForm(g_styleIndex);
            if (!style) {
                TextColored(kErrorRed, "Combat style %d not found in LazyFollowers.esp.", g_styleIndex + 1);
                return;
            }
            if (g_styleNameFor != g_styleIndex) {
                strncpy_s(g_styleName.data(), g_styleName.size(), snap.styleNames[g_styleIndex].c_str(), _TRUNCATE);
                g_styleNameFor = g_styleIndex;
            }

            Section("Combat Style", "The 10 combat styles followers can use (Followers page > Combat Style).\nChanges are kept in the save; Settings > Save Config writes them to a file.",
                false);
            BeginRow("Combat Style");
            if (const int p = SearchCombo("##v", snap.styleNames[g_styleIndex].c_str(), LF::kStyles, g_styleIndex,
                    [&](int i) -> std::string_view { return snap.styleNames[i]; });
                p >= 0) {
                g_styleIndex = p;
            }
            EndRowNoReset();
            BeginRow("Style Name", "Type a name and press Enter.");
            if (InputText("##v", g_styleName.data(), g_styleName.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
                LF::SetStyleName(g_styleIndex, g_styleName.data());
            }
            EndRowNoReset();

            auto       v = LF::ReadStyle(style);
            const auto def = LF::StyleDefaults(g_styleIndex);
            Section("General");
            StyleSlider(style, v, def, 0, "Offensive Mult", 1.0f);
            StyleSlider(style, v, def, 1, "Defensive Mult", 1.0f);
            StyleSlider(style, v, def, 2, "Group Offensive Mult", 1.0f);
            StyleSlider(style, v, def, 3, "Avoid Threat Chance", 1.0f);
            StyleSlider(style, v, def, 4, "Melee Mult", 10.0f);
            StyleSlider(style, v, def, 5, "Ranged Mult", 10.0f);
            StyleSlider(style, v, def, 6, "Magic Mult", 10.0f);
            StyleSlider(style, v, def, 7, "Shout Mult", 10.0f);
            StyleSlider(style, v, def, 8, "Staff Mult", 10.0f);
            StyleSlider(style, v, def, 9, "Unarmed Mult", 10.0f);

            Section("Ranged");
            StyleSlider(style, v, def, 10, "Close Range Dueling Circle", 1.0f);
            StyleSlider(style, v, def, 11, "Close Range Dueling Fallback", 1.0f);
            StyleSlider(style, v, def, 12, "Close Range Flank Distance", 1.0f);
            StyleSlider(style, v, def, 13, "Close Range Stalk Time", 1.0f);
            StyleSlider(style, v, def, 14, "Long Range Strafe Mult", 1.0f);

            Section("Melee");
            StyleSlider(style, v, def, 15, "Attack Staggered", 10.0f);
            StyleSlider(style, v, def, 16, "Power Attack Staggered", 10.0f);
            StyleSlider(style, v, def, 17, "Power Attack Blocking", 10.0f);
            StyleSlider(style, v, def, 18, "Bash", 10.0f);
            StyleSlider(style, v, def, 19, "Bash Recoiled", 10.0f);
            StyleSlider(style, v, def, 20, "Bash Attack", 10.0f);
            StyleSlider(style, v, def, 21, "Bash Power Attack", 10.0f);
            StyleSlider(style, v, def, 22, "Special Attack", 1.0f);
            if (BeginRow("Allow Dual Wielding")) {
                bool on = v[23] != 0.0f;
                bool changed = Checkbox("##v", &on);
                if (EndRow((v[23] != 0.0f) == (def[23] != 0.0f))) {
                    on = def[23] != 0.0f;
                    changed = true;
                }
                if (changed) {
                    v[23] = on ? 1.0f : 0.0f;
                    LF::WriteStyle(style, v);
                }
            }

            if (ResetAllButton("Every value of this combat style and its name back to the defaults.",
                    std::format("Reset all values of {} to the defaults?", snap.styleNames[g_styleIndex]))) {
                LF::ResetStyle(g_styleIndex);
                g_styleNameFor = -1;  // reload the name box
            }
            NoMatch();
        }

        // ------------------------------------------------------------------
        // Settings page (MCM page " Debug" + " SaveLoad" + Save Actor Preset)
        // ------------------------------------------------------------------
        RE::FormID           g_pickRef = 0;  // the ticked actor in Nearby Actors (one at a time)
        RE::FormID           g_seenConsole = 0;    // console selection / last crosshair actor already ticked once
        RE::FormID           g_seenCrosshair = 0;

        // NG (user: the Add buttons did nothing; then "console target before crosshair"): a NEW console selection is
        // put in the list and ticked; a NEW actor looked at only while nothing is selected in the console. Each once,
        // so a row ticked by hand afterwards sticks. Called when the list draws and by the Open Inventory hotkey.
        // a_crosshair counts only when a_useCrosshair (NG fix, user video 10-05-14: in the Nearby Actors window the game
        // runs, the crosshair kept passing over followers and the tick jumped back to them): when the page / window
        // opens, and for the hotkey while the Nearby Actors window is closed.
        void UpdatePick(RE::FormID a_console, RE::FormID a_crosshair, bool a_consoleBusy, bool a_useCrosshair)
        {
            if (a_console == 0x14) {
                a_console = 0;  // you: not a target
            }
            if (a_crosshair == 0x14) {
                a_crosshair = 0;
            }
            if (a_console && a_console != g_seenConsole) {
                LF::AddNearby(a_console);
                g_pickRef = a_console;
            } else if (a_useCrosshair && !a_console && !a_consoleBusy && a_crosshair && a_crosshair != g_seenCrosshair) {
                LF::AddNearby(a_crosshair);
                g_pickRef = a_crosshair;
            }
            g_seenConsole = a_console;
            if (a_useCrosshair) {
                g_seenCrosshair = a_crosshair;  // not seen while ignored: still new when the list opens again
            }
        }
        std::array<char, 64> g_nearbySearch{};
        std::array<char, 80> g_presetName{};

        // "Nearby Actors" like Actor Control's (one actor at a time, Actor + Distance columns): radius bar +
        // "Scan radius" + Scan nearby NPCs + Reset, then the search box, "n of m selected", the table (grip
        // under it) - and, merged in (user), Save Actor Preset for the ticked actor. The console selection and
        // the last actor looked at are added and ticked by themselves (user).
        // a_opened: the page / window was just opened (the actor looked at may be ticked then).
        void DrawNearbyActors(const LF::Snapshot& a_snap, bool a_opened)
        {
            Section("Nearby Actors", "Scan the area and tick the actor Save Actor Preset saves (one at a time).\n"
                                     "An NPC you click with the console open is added and ticked by itself; so is the last NPC you looked at\n"
                                     "while nothing is selected in the console (the console selection comes first).\n"
                                     "The Open Inventory hotkey (Settings > Hotkey) opens the ticked NPC's inventory.\n"
                                     "You are always first in the list. Opening this page scans once, and so does\n"
                                     "loading a save or entering a new cell (2 seconds later, when the NPCs there have loaded).\n\n"
                                     "Save Actor Preset writes the actor's RaceMenu look as Data/SKSE/Plugins/CharGen/Exported/<name>.jslot\n"
                                     "(the file \"skee preset-save <name>\" makes - it shows up in the Followers page's Preset list)\n"
                                     "and its face tint as Data/Textures/CharGen/Exported/<name>.dds. No console needed.");
            if (!ShowGroup("Nearby Actors Scan radius Scan nearby NPCs Save Actor Preset Preset Name")) {
                return;
            }
            // the radius bar first, its name right of it, then Scan and Reset (as in Actor Control)
            {
                PushID("Scan radius");
                float       r = LF::ScanRadius();
                const float barWidth = std::clamp(GetContentRegionAvail().x - 8.0f - kResetWidth, 60.0f, kMaxControlWidth);
                SetNextItemWidth(barWidth);
                SliderFloat("##radius", &r, 1.0f, 100.0f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
                if (IsItemDeactivatedAfterEdit()) {
                    LF::SetScanRadius(r);
                }
                Tooltip("How far around you Scan nearby NPCs looks.");
                NextToWidth(CalcTextSize("Scan radius").x);
                AlignTextToFramePadding();
                TextUnformatted("Scan radius");
                const bool scanning = LF::Scanning();
                NextTo("Scan nearby NPCs");
                BeginDisabled(scanning);
                if (Button(scanning ? "Scanning..." : "Scan nearby NPCs")) {
                    LF::ScanNearby();
                }
                EndDisabled();
                Tooltip("Scans now. It also scans by itself when you open this page, load a save or enter a new cell -\n"
                        "one quick pass each time, nothing runs in between. Only NPCs loaded near you are checked.");
                const bool isDefault = LF::ScanRadius() == LF::kDefaultScanRadius;
                NextToWidth(kResetWidth);
                BeginDisabled(isDefault);
                if (Button("Reset", ImVec2{ kResetWidth, 0.0f })) {
                    LF::SetScanRadius(LF::kDefaultScanRadius);
                }
                EndDisabled();
                if (!isDefault && IsItemHovered()) {
                    SolidTooltip("Back to the default");
                }
                PopID();
            }

            if (a_opened) {
                // just opened: the snapshot may be from before (it is only rebuilt while a page is up) - read them now
                const auto now = LF::PickRefs();
                UpdatePick(now.console, now.crosshair, now.consoleBusy, true);
            } else {
                UpdatePick(a_snap.consoleRef, a_snap.crosshairRef, a_snap.consoleBusy, false);
            }
            const RE::FormID pickBefore = g_pickRef;
            const auto all = LF::Nearby();
            const bool ticked = std::any_of(all.begin(), all.end(), [](const LF::NearbyActor& a_n) { return a_n.id == g_pickRef; });
            SetNextItemWidth(340.0f);
            InputTextWithHint("##nearbysearch", "Search name or ID...", g_nearbySearch.data(), g_nearbySearch.size());
            if (g_nearbySearch[0]) {
                SameLine();
                if (SmallButton("x##clearnearby")) {
                    g_nearbySearch[0] = '\0';
                }
            }
            const auto selected = std::format("{} of {} selected", ticked ? 1 : 0, all.size());
            NextToWidth(CalcTextSize(selected.c_str()).x);
            AlignTextToFramePadding();
            TextUnformatted(selected.c_str());

            if (all.empty()) {
                TextDisabled("No NPCs found yet.");
                Tooltip("Press Scan nearby NPCs.");
            } else {
                std::vector<const LF::NearbyActor*> shown;
                for (auto& n : all) {
                    if (Contains(n.name, g_nearbySearch.data()) || Contains(std::format("{:08X}", n.id), g_nearbySearch.data())) {
                        shown.push_back(&n);
                    }
                }
                int rows = LF::NearbyRows();  // the saved value (the grip saves each change, Reset All resets it)
                const std::size_t count = std::max<std::size_t>(shown.size(), 1);
                if (BeginListTable("##nearby2", 2, count, GetFrameHeight(), 300.0f + 90.0f, static_cast<std::size_t>(rows))) {
                    TableSetupColumn("Actor", ImGuiTableColumnFlags_WidthFixed, 300.0f);
                    TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                    static ColumnWidths s_cols;
                    KeepColumnWidths(s_cols, LF::NearbyWidths, LF::SetNearbyWidths);
                    TableHeadersRow();
                    for (auto* n : shown) {
                        PushID(static_cast<int>(n->id));
                        TableNextRow();
                        TableNextColumn();
                        bool       on = n->id == g_pickRef;
                        const auto label = std::format("{}  [{:08X}]", n->name, n->id);
                        if (Checkbox(label.c_str(), &on)) {
                            g_pickRef = on ? n->id : 0;  // one actor at a time
                        }
                        TableNextColumn();
                        AlignTextToFramePadding();
                        TextUnformatted(std::format("{:.1f} m", n->meters).c_str());
                        PopID();
                    }
                    if (shown.empty()) {
                        TableNextRow();
                        TableNextColumn();
                        TextDisabled("No match for \"%s\"", g_nearbySearch.data());
                    }
                    if (TableGetHoveredColumn() >= 0 && IsMouseClicked(ImGuiMouseButton_Right)) {
                        OpenPopup("##nearbyctx");  // right-click on the table: Select none
                    }
                    if (BeginPopup("##nearbyctx")) {
                        if (Selectable("Select none")) {
                            g_pickRef = 0;
                        }
                        EndPopup();
                    }
                    EndTable();
                    ListTableGrip("##nearbygrip", rows, count, GetFrameHeight(), LF::SetNearbyRows);
                }
            }

            // NG (user video 08-19-05): another actor ticked while the inventory Open Inventory opened is up -> it switches
            if (g_pickRef != pickBefore && g_pickRef) {
                if (const auto open = LF::InventoryOpenFor(); open && open != g_pickRef) {
                    LF::OpenInventory(g_pickRef);
                }
            }

            // Save Actor Preset (merged into this section)
            std::string target;
            for (auto& n : all) {
                if (n.id == g_pickRef) {
                    target = n.name;
                }
            }
            Spacing();
            if (BeginRow("Preset Name", "The file name (no extension). Characters like \\ / : * ? are removed.")) {
                InputTextWithHint("##v", "Name", g_presetName.data(), g_presetName.size());
                EndRowNoReset();
            }
            const auto      file = Files::SafeFileName(g_presetName.data());
            const bool      can = a_snap.raceMenu && !file.empty() && !target.empty();
            std::error_code ec;
            const bool      exists = !file.empty() && std::filesystem::exists(Files::PresetFolder() / (file + ".jslot"), ec);
            const auto      question = exists ? std::format("{}.jslot already exists. Overwrite it with {}'s look?", file, target)
                                              : std::format("Save {}'s look as {}.jslot?", target, file);
            const char*     tip = !a_snap.raceMenu ? "Needs RaceMenu." : target.empty() ? "Tick an actor in the list first." :
                                  file.empty() ? "Type a preset name first." : "Saves the ticked actor's look (RaceMenu's CharGen.SaveCharacterPreset).";
            if (ConfirmButton("Save Actor Preset", tip, question, !can)) {
                LF::SavePreset(g_pickRef, g_presetName.data());
            }
            if (!a_snap.raceMenu) {
                SameLine();
                TextColored(kErrorRed, "RaceMenu not found");
            }
        }

        bool GlobalSlider(const char* a_label, LF::Global a_g, int a_min, int a_max, const char* a_format, const char* a_tip)
        {
            bool done = false;
            if (!BeginRow(a_label, a_tip)) {
                return false;
            }
            int v = LF::GetGlobal(a_g);
            if (SliderInt("##v", &v, a_min, a_max, a_format, ImGuiSliderFlags_AlwaysClamp)) {
                LF::SetGlobal(a_g, v, false);  // NG cleanup: the file is written once, on release
            }
            done = IsItemDeactivatedAfterEdit();
            if (done) {
                LF::SaveSettingsFile();
            }
            if (EndRow(v == LF::GlobalDefault(a_g))) {
                LF::SetGlobal(a_g, LF::GlobalDefault(a_g));
                done = true;
            }
            return done;
        }

        // A Settings checkbox row with Reset (default on).
        void FlagRow(const char* a_label, const char* a_tip, bool a_value, void (*a_set)(bool))
        {
            if (!BeginRow(a_label, a_tip)) {
                return;
            }
            bool v = a_value;
            bool changed = Checkbox("##v", &v);
            if (EndRow(v)) {
                v = true;
                changed = true;
            }
            if (changed) {
                a_set(v);
            }
        }

        constexpr const char* kModesTip =
            "RaceMenu flags (add them up):\n"
            "0 Default (full)\n1 Overrides\n2 Body\n3 Overrides + Body\n4 Transforms\n5 Overrides + Transforms\n6 Body + Transforms\n"
            "7 Overrides + Body + Transforms\n8 Skin\n11 Skin + Body + Overrides\n13 Overrides + Transforms + Skin\n15 All";

        std::map<Hotkey::Key, LF::KeyBinding> g_keyAtStart;  // each hotkey as the settings file had it when the game started

        // A Hotkey section row: the key button (click, then press a key) + Clear (no key, greyed when none) + Reset
        // (NG, user: "clear and reset button") = back to the key it had when the game started (greyed when unchanged).
        void HotkeyRow(const char* a_label, const char* a_tip, Hotkey::Key a_which, LF::KeyBinding a_key, void (*a_set)(LF::KeyBinding))
        {
            const auto start = g_keyAtStart.try_emplace(a_which, a_key).first->second;  // first draw = the loaded key
            if (!BeginRow(a_label, a_tip)) {
                return;
            }
            const bool capturing = Hotkey::Capturing(a_which);
            const auto label = capturing ? std::string("Press a key...") : Hotkey::Name(a_key);
            if (Button((label + "###key").c_str(), ImVec2{ g_rowWidth, 0.0f })) {
                if (capturing) {
                    Hotkey::CancelCapture();
                } else {
                    Hotkey::BeginCapture(a_which);
                }
            }
            Tooltip(g_rowTip);  // the row's help on the key button (EndRow would put it on Clear)
            g_rowTip = nullptr;
            NextToWidth(kResetWidth);
            BeginDisabled(!a_key.IsSet() && !capturing);
            if (Button("Clear", ImVec2{ kResetWidth, 0.0f })) {
                if (capturing) {
                    Hotkey::CancelCapture();
                }
                a_set({});
            }
            EndDisabled();
            if (a_key.IsSet() && IsItemHovered()) {
                SolidTooltip("No key");
            }
            const bool unchanged = a_key.vk == start.vk && a_key.mods == start.mods;
            const auto resetTip = std::format("Back to {} (the key when the game started)", Hotkey::Name(start));
            if (EndRow(unchanged, resetTip.c_str())) {
                if (capturing) {
                    Hotkey::CancelCapture();
                }
                a_set(start);
            }
        }

        std::set<std::string> g_presetSel;  // Settings > Presets: the ticked files ("<name>.jslot" / "<name>.jslot.hidden")
        std::array<char, 64>  g_presetSearch{};
        std::string           g_presetNotice;  // NG (user): the last Hide / Unhide / Delete result, next to "x of y selected"
        bool                  g_presetNoticeError = false;
        int                   g_presetAsk = 0;  // NG: right-click Hide (1) / Delete (2) picked - its Yes / No dialog opens next

        // The Presets section's result line (also in the log).
        void PresetNotice(std::string a_text, bool a_error = false)
        {
            if (a_error) {
                logger::warn("{}", a_text);
            } else {
                logger::info("{}", a_text);
            }
            g_presetNotice = std::move(a_text);
            g_presetNoticeError = a_error;
        }

        // The result text (user: "x of <selected> presets are hidden", nothing more - details go to the log).
        std::string PresetResult(int a_done, std::size_t a_of, const char* a_what)
        {
            return a_of == 1 ? std::format("{} of 1 preset is {}", a_done, a_what) : std::format("{} of {} presets are {}", a_done, a_of, a_what);
        }

        // NG (user: "Hide, Unhide, Delete files - and multiple"): Settings > Presets - every RaceMenu preset of
        // CharGen/Exported in one table (shown + hidden, a search box, tick any number; hidden rows greyed out). Right-click
        // menu (user: instead of buttons): select all shown / none, Hide / Unhide / Delete on the ticked ones. Hide renames
        // the .jslot, .dds tint and .nif head mesh to <file>.hidden (nothing deleted); Unhide renames them back; Delete
        // removes them for good. Hide and Delete ask first and say which followers use the presets. The result
        // ("x of y presets are hidden") stands next to the "x of y selected" count; details only in the log.
        void DrawHidePreset(const LF::Snapshot& a_snap)
        {
            Section("Presets", "RaceMenu presets in Data/SKSE/Plugins/CharGen/Exported (the Followers page's Preset list).\n"
                               "Tick presets in the table, then right-click it (also: select all shown / none):\n"
                               "Hide - renames <name>.jslot, its tint Textures/CharGen/Exported/<name>.dds and head mesh <name>.nif\n"
                               "to <file>.hidden: gone from the lists, nothing deleted. Unhide - brings hidden ones back.\n"
                               "Delete - removes the files for good (MO2: from the mod or Overwrite folder they are in).");
            if (!ShowGroup("Presets Preset Hide Unhide Delete Hidden")) {
                return;
            }
            struct Row
            {
                std::string file;    // as on disk
                std::string name;    // without .jslot / .hidden
                bool        hidden;
            };
            std::vector<Row> rows;
            for (const auto& f : a_snap.lists->presets) {
                rows.push_back({ f, f.size() > 6 ? f.substr(0, f.size() - 6) : f, false });
            }
            for (const auto& f : a_snap.lists->hiddenPresets) {
                const auto pos = f.find(".jslot");
                rows.push_back({ f, pos == std::string::npos ? f : f.substr(0, pos), true });
            }
            std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) { return _stricmp(x.name.c_str(), y.name.c_str()) < 0; });
            // ticks of files that are gone (renamed / deleted) are dropped
            std::erase_if(g_presetSel, [&](const std::string& f) { return std::none_of(rows.begin(), rows.end(), [&](const Row& r) { return r.file == f; }); });

            // followers using a (shown) preset
            auto usersOf = [&](const Row& r) {
                std::string users;
                if (!r.hidden) {
                    for (int i = 0; i < static_cast<int>(a_snap.slots.size()); ++i) {
                        if (a_snap.slots[i].Active() && a_snap.slots[i].preset == r.name) {
                            users += (users.empty() ? "" : ", ") + SlotLabel(a_snap.slots[i], i);
                        }
                    }
                }
                return users;
            };

            SetNextItemWidth(340.0f);
            InputTextWithHint("##presetsearch", "Search presets...", g_presetSearch.data(), g_presetSearch.size());
            if (g_presetSearch[0]) {
                SameLine();
                if (SmallButton("x##clearpresets")) {
                    g_presetSearch[0] = '\0';
                }
            }
            const auto count = std::format("{} of {} selected", g_presetSel.size(), rows.size());
            NextToWidth(CalcTextSize(count.c_str()).x);
            AlignTextToFramePadding();
            TextUnformatted(count.c_str());
            // the result of the last Hide / Unhide / Delete, next to the count (user)
            if (!g_presetNotice.empty()) {
                NextToWidth(CalcTextSize(g_presetNotice.c_str()).x);
                AlignTextToFramePadding();
                TextColored(g_presetNoticeError ? kErrorRed : kValueBlue, "%s", g_presetNotice.c_str());  // like DrawNotice
            }

            // the ticked ones, split by what each menu entry can do
            std::vector<const Row*> toHide, toUnhide, toDelete;
            std::string             users;
            for (const auto& r : rows) {
                if (!g_presetSel.contains(r.file)) {
                    continue;
                }
                toDelete.push_back(&r);
                (r.hidden ? toUnhide : toHide).push_back(&r);
                if (const auto u = usersOf(r); !u.empty()) {
                    users += std::format("\n  {}: {}", r.name, u);
                }
            }
            auto names = [](const std::vector<const Row*>& a_list) {
                std::string out;
                for (std::size_t i = 0; i < a_list.size() && i < 12; ++i) {
                    out += "\n  " + a_list[i]->name;
                }
                if (a_list.size() > 12) {
                    out += std::format("\n  ... and {} more", a_list.size() - 12);
                }
                return out;
            };
            const std::string usedBy = users.empty() ? std::string{} : "\nUsed by followers (their look needs the preset when they load again):" + users;

            std::vector<const Row*> shown;
            for (const auto& r : rows) {
                if (Contains(r.name, g_presetSearch.data())) {
                    shown.push_back(&r);
                }
            }
            if (rows.empty()) {
                TextDisabled("No presets in CharGen/Exported");
            } else if (int presetRows = LF::PresetRows();  // the saved value (the grip saves each change)
                       BeginListTable("##presets", 3, std::max<std::size_t>(shown.size(), 1), GetFrameHeight(), 280.0f + 70.0f + 220.0f,
                           static_cast<std::size_t>(presetRows))) {
                TableSetupColumn("Preset", ImGuiTableColumnFlags_WidthFixed, 280.0f);
                TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                TableSetupColumn("Used by", ImGuiTableColumnFlags_WidthFixed, 220.0f);
                static ColumnWidths s_cols;  // NG (user): resizable like the Nearby Actors table, widths kept
                KeepColumnWidths(s_cols, LF::PresetWidths, LF::SetPresetWidths);
                TableHeadersRow();
                ImVec4 grey{ 0.5f, 0.5f, 0.5f, 1.0f };
                if (auto c = GetStyleColorVec4(ImGuiCol_TextDisabled)) {
                    grey = *c;
                }
                for (auto* r : shown) {
                    PushID(r->file.c_str());
                    if (r->hidden) {
                        PushStyleColor(ImGuiCol_Text, grey);  // NG (user): a hidden preset's whole row greyed out (still tickable)
                    }
                    TableNextRow();
                    TableNextColumn();
                    bool on = g_presetSel.contains(r->file);
                    if (Checkbox(r->name.c_str(), &on)) {
                        if (on) {
                            g_presetSel.insert(r->file);
                        } else {
                            g_presetSel.erase(r->file);
                        }
                    }
                    TableNextColumn();
                    AlignTextToFramePadding();
                    TextUnformatted(r->hidden ? "Hidden" : "Shown");
                    TableNextColumn();
                    AlignTextToFramePadding();
                    TextUnformatted(usersOf(*r).c_str());
                    if (r->hidden) {
                        PopStyleColor();
                    }
                    PopID();
                }
                if (shown.empty()) {
                    TableNextRow();
                    TableNextColumn();
                    TextDisabled("No match for \"%s\"", g_presetSearch.data());
                }
                if (TableGetHoveredColumn() >= 0 && IsMouseClicked(ImGuiMouseButton_Right)) {
                    OpenPopup("##presetctx");
                }
                if (BeginPopup("##presetctx")) {
                    if (Selectable("Select all shown")) {
                        for (auto* r : shown) {
                            g_presetSel.insert(r->file);
                        }
                    }
                    if (Selectable("Select none")) {
                        g_presetSel.clear();
                    }
                    Separator();
                    const auto hideLabel = std::format("Hide ({})", toHide.size());
                    if (Selectable(hideLabel.c_str(), false, toHide.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
                        g_presetAsk = 1;
                    }
                    Tooltip(toHide.empty() ? "Tick shown presets first." : "Renames the ticked presets' .jslot, .dds tint and .nif head mesh to <file>.hidden.");
                    const auto unhideLabel = std::format("Unhide ({})", toUnhide.size());
                    if (Selectable(unhideLabel.c_str(), false, toUnhide.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
                        int done = 0;
                        for (auto* r : toUnhide) {
                            std::string err;
                            if (Files::UnhidePreset(r->file, err)) {
                                ++done;
                            } else {
                                logger::warn("Unhide {}: {}", r->name, err);
                            }
                        }
                        PresetNotice(PresetResult(done, toUnhide.size(), "unhidden"), done != static_cast<int>(toUnhide.size()));
                        g_presetSel.clear();
                        LF::RebuildLists();
                    }
                    Tooltip(toUnhide.empty() ? "Tick hidden presets first." : "Renames the ticked hidden presets back to .jslot (and their .dds tint / .nif head mesh).");
                    const auto deleteLabel = std::format("Delete ({})", toDelete.size());
                    if (Selectable(deleteLabel.c_str(), false, toDelete.empty() ? ImGuiSelectableFlags_Disabled : 0)) {
                        g_presetAsk = 2;
                    }
                    Tooltip(toDelete.empty() ? "Tick presets first." : "Deletes the ticked presets for good (.jslot + .dds tint + .nif head mesh).");
                    EndPopup();
                }
                EndTable();
                ListTableGrip("##presetgrip", presetRows, std::max<std::size_t>(shown.size(), 1), GetFrameHeight(), LF::SetPresetRows);
            }

            // the menu's Hide / Delete ask first (opened here, outside the menu popup, at the dialogs' own ID level)
            if (g_presetAsk == 1) {
                OpenPopup("Hide presets##presetHideDlg");
            } else if (g_presetAsk == 2) {
                OpenPopup("Delete presets##presetDeleteDlg");
            }
            g_presetAsk = 0;
            if (ConfirmDialog("Hide presets##presetHideDlg",
                    std::format("Hide {} preset(s)? Their .jslot, .dds and .nif get \".hidden\" added (nothing is deleted).{}{}", toHide.size(), names(toHide), usedBy))) {
                int done = 0;
                for (auto* r : toHide) {
                    done += Files::HidePreset(r->file) ? 1 : 0;
                }
                PresetNotice(PresetResult(done, toHide.size(), "hidden"), done != static_cast<int>(toHide.size()));
                g_presetSel.clear();
                LF::RebuildLists();
            }
            if (ConfirmDialog("Delete presets##presetDeleteDlg",
                    std::format("DELETE {} preset(s) for good? Their .jslot, .dds and .nif are removed - this can't be undone\n"
                                "(Hide keeps them instead).{}{}",
                        toDelete.size(), names(toDelete), usedBy))) {
                int done = 0;
                for (auto* r : toDelete) {
                    std::string err;
                    if (Files::DeletePreset(r->file, err)) {
                        ++done;
                    } else {
                        logger::warn("Delete {}: {}", r->name, err);
                    }
                }
                PresetNotice(PresetResult(done, toDelete.size(), "deleted"), done != static_cast<int>(toDelete.size()));
                g_presetSel.clear();
                LF::RebuildLists();
            }
        }

        void __stdcall RenderSettings()
        {
            const bool opened = NoteDrawn(4);
            if (opened) {
                g_presetNotice.clear();  // NG (user): the Presets result goes away once the menu was closed
            }
            BeginPage("settings", true);
            auto snapOpt = Ready();
            if (!snapOpt) {
                return;
            }
            const auto& snap = *snapOpt;
            if (opened) {
                LF::ScanNearby();
            }

            DrawNotice();

            DrawNearbyActors(snap, opened);

            Section("RaceMenu", kModesTip);
            // NG (user): Apply Delay removed (a load pass waits for the follower's 3D itself; fixed 1 s after it loads)
            // NG (user): RM Full Mode + RM Lite Mode merged into one setting
            GlobalSlider("RaceMenu Mode", LF::Global::kLiteMode, 0, 15, "%d",
                (std::string("RaceMenu flags for every preset apply: Apply Settings / Apply to All / NPC Preset and whenever a follower loads.\n"
                             "Body (2) is only applied right after Apply Settings / Apply to All / NPC Preset, so a body changed later stays.\n\n") +
                    kModesTip)
                    .c_str());

            Section("Behaviour");
            if (BeginRow("Sandboxing", "Followers not following you use the default sandbox package when idle.\nTurn off if another mod handles them.")) {
                bool v = LF::GetGlobal(LF::Global::kSandbox) != 0;
                bool changed = Checkbox("##v", &v);
                if (EndRow(v == (LF::GlobalDefault(LF::Global::kSandbox) != 0))) {
                    v = LF::GlobalDefault(LF::Global::kSandbox) != 0;
                    changed = true;
                }
                if (changed) {
                    LF::SetGlobal(LF::Global::kSandbox, v ? 1 : 0);
                }
            }
            FlagRow("Custom Combat Styles", "Followers use the Lazy Followers combat style picked on the Followers page.\nOff also greys out the Combat Styles page.",
                LF::StylesEnabled(), LF::SetStylesEnabled);
            FlagRow("Custom Classes", "Followers use the class picked on the Followers page.", LF::ClassesEnabled(), LF::SetClassesEnabled);
            FlagRow("Custom Spells", "Off greys out the Spells page. Spells already added stay on the followers.",
                LF::SpellsEnabled(), LF::SetSpellsEnabled);

            std::error_code ec;
            Section("Config Data", "Voices, races and the 10 combat styles (Data/SKSE/Plugins/LazyFollowers/LazyFollowersConfig.json).");
            if (Show("Load Config Save Config")) {
                if (std::filesystem::exists(Files::ConfigFile(), ec)) {
                    if (ConfirmButton("Load Config", "Loads LazyFollowersConfig.json (adds its voices / races, replaces the combat styles).",
                            "LOAD config from file? Current combat styles will be OVERWRITTEN.")) {
                        LF::LoadConfig();
                    }
                    NextTo("Save Config");
                }
                if (ConfirmButton("Save Config", "File -> Data/SKSE/Plugins/LazyFollowers/LazyFollowersConfig.json", "SAVE config to file? Existing config data will be OVERWRITTEN.")) {
                    LF::SaveConfig();
                }
            }

            DrawHidePreset(snap);

            Section("NPC Data", "Followers' name, preset, race, voice, sex, style, class, spells and worn gear.");
            if (Show("Load All NPCs Save All NPCs to One File Save Each NPC Separately")) {
                if (std::filesystem::exists(Files::NPCFile(), ec)) {
                    if (ConfirmButton("Load All NPCs", "Loads LazyFollowersNPC.json into the slots it was saved from.",
                            "LOAD followers from file? All current followers will be reset and sent back to storage first.")) {
                        g_pending.clear();
                        LF::LoadAllNPCs();
                    }
                    NextTo("Save All NPCs to One File");
                }
                if (ConfirmButton("Save All NPCs to One File", "File -> Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPC.json", "SAVE followers to file? ALL existing NPC data will be OVERWRITTEN.")) {
                    LF::SaveAllNPCs();
                }
                NextTo("Save Each NPC Separately");
                if (ConfirmButton("Save Each NPC Separately", "Stored in -> Data/SKSE/Plugins/LazyFollowers/LazyFollowersNPCs/<name>.json\n(load them on the Followers page > NPC Preset)",
                        "Save each follower to its own file? Naming convention - followerName.json")) {
                    LF::SaveEachNPC();
                }
            }

            // NG 1.1 (user): hotkeys for the Lazy Followers window (as Actor Control's) and Summon Follower
            Section("Hotkey", "Keys are read from Windows, so they also work over other menus.\n"
                              "Click the key button, then press a key (Ctrl / Shift / Alt held with it count). Esc cancels.");
            if (static bool fixed = false; !fixed && LF::InGame()) {
                Hotkey::FixShared();  // one key per hotkey (older settings files)
                fixed = true;
            }
            HotkeyRow("Open Window", "Opens / closes the Lazy Followers pages in a window of their own (tabs),\n"
                                     "without opening the SKSE menu. Esc or the window's X closes it.",
                Hotkey::Key::kOpenWindow, LF::OpenWindowKey(), LF::SetOpenWindowKey);
            HotkeyRow("Summon Follower", "Summons the follower selected on the Followers page to you (like Summon NPC).\n"
                                         "No follower selected yet: the first one in the Follower list.",
                Hotkey::Key::kSummon, LF::SummonKey(), LF::SetSummonKey);
            HotkeyRow("Summon All", "Summons every follower that is not following you (like the Summon All button).",
                Hotkey::Key::kSummonAll, LF::SummonAllKey(), LF::SetSummonAllKey);
            HotkeyRow("Save Player Preset", "Asks for a name, then saves YOUR look as a preset (like the console's skee preset-save <name>):\n"
                                            "the .jslot + the face tint .dds in the CharGen Exported folders, so it shows in the Preset list.\n"
                                            "Enter saves, Esc cancels.",
                Hotkey::Key::kSavePreset, LF::SavePresetKey(), LF::SetSavePresetKey);
            HotkeyRow("Open Inventory", "Opens the inventory of the NPC ticked in Nearby Actors (take / give items, like a follower's trade\n"
                                        "window - any living NPC near you). A new console selection, or a new NPC looked at while nothing is\n"
                                        "selected in the console, is ticked first. You ticked: your own inventory. The Nearby Actors window\n"
                                        "stays open - tick another actor there to switch. Closes the Lazy Followers window first.",
                Hotkey::Key::kOpenInventory, LF::OpenInventoryKey(), LF::SetOpenInventoryKey);
            HotkeyRow("Nearby Actors Window", "Opens / closes Nearby Actors (scan, the actor list, Save Actor Preset) in a small window of its own,\n"
                                              "without the SKSE menu. Opening it scans once. Esc or the window's X closes it.",
                Hotkey::Key::kNearbyWindow, LF::NearbyWindowKey(), LF::SetNearbyWindowKey);

            Section("Logging");
            // NG (user): a toggle instead of the Logging Mode drop-down. On = everything in the log file (global 1),
            // off = warnings and errors only (global 2). The old "Console + log file" (0) counts as on.
            if (BeginRow("Logging", "On: everything Lazy Followers does goes into the log file.\nOff: only warnings and errors.\n"
                                    "Log: Documents/My Games/Skyrim Special Edition/SKSE/LazyFollowersNG.log")) {
                bool v = LF::GetGlobal(LF::Global::kTrace) < 2;
                bool changed = Checkbox("##v", &v);
                if (EndRow(v == (LF::GlobalDefault(LF::Global::kTrace) < 2))) {
                    v = LF::GlobalDefault(LF::Global::kTrace) < 2;
                    changed = true;
                }
                if (changed) {
                    LF::SetGlobal(LF::Global::kTrace, v ? 1 : 2);
                }
            }

            if (ResetAllButton("Every value on this page (RaceMenu mode, behaviour, logging, scan radius) back to its default.\n"
                               "Files, presets, followers and the hotkey are not touched.",
                    "Reset all settings to their defaults?")) {
                LF::ResetSettings();
            }
            NoMatch();
        }

    }

    namespace
    {
        // Opens / closes one of our windows. NG fix (Save Player Preset lock, found with Frida): a hotkey's key
        // press can also reach the game as one of its controls (here Toggle POV) and the window then takes the key
        // release - so on close the game's "Toggle POV held" state is released (LF::ResetPovHold).
        void SetWindowOpen(SKSEMenuFramework::Model::WindowInterface* a_window, bool a_open)
        {
            if (!a_window) {
                return;
            }
            const bool was = a_window->IsOpen.exchange(a_open);
            if (was && !a_open) {
                LF::ResetPovHold();
            }
        }

        SKSEMenuFramework::Model::WindowInterface* g_window = nullptr;  // the Lazy Followers window (AddWindow)

        // The Lazy Followers window, drawn every frame by SKSE Menu Framework while it is open: the five pages
        // as tabs. Its X or Esc closes it.
        void __stdcall RenderWindow()
        {
            if (!g_window || !g_window->IsOpen.load()) {
                return;
            }
            if (IsKeyPressed(ImGuiKey_Escape, false) && !Hotkey::Capturing()) {
                SetWindowOpen(g_window, false);
                return;
            }
            SetNextWindowPos(ImVec2{ 80.0f, 60.0f }, ImGuiCond_FirstUseEver);
            SetNextWindowSize(ImVec2{ 760.0f, 680.0f }, ImGuiCond_FirstUseEver);
            bool open = true;
            if (Begin("Lazy Followers###LFNGWindow", &open, ImGuiWindowFlags_NoCollapse)) {
                static int page = 0;
                if (BeginTabBar("##lfngtabs")) {
                    static const char* const kTabs[] = { "Followers", "Stats", "Spells", "Combat Styles", "Settings" };
                    for (int i = 0; i < 5; ++i) {
                        if (BeginTabItem(kTabs[i])) {
                            page = i;
                            EndTabItem();
                        }
                    }
                    EndTabBar();
                }
                // the page in its own scroll area (its search box, scrollbar jump and folder box use it)
                BeginChild("##lfngpage", ImVec2{ 0.0f, 0.0f });
                switch (page) {
                case 0:
                    RenderFollowers();
                    break;
                case 1:
                    RenderStats();
                    break;
                case 2:
                    RenderSpells();
                    break;
                case 3:
                    RenderStyles();
                    break;
                default:
                    RenderSettings();
                    break;
                }
                EndChild();
            }
            End();
            if (!open) {
                SetWindowOpen(g_window, false);
            }
        }
    }

    namespace
    {
        SKSEMenuFramework::Model::WindowInterface* g_saveWindow = nullptr;  // the Save Player Preset dialog (AddWindow)
        std::array<char, 80>                       g_playerPresetName{};
        bool                                       g_saveFocus = false;  // put the keyboard in the name box (just opened)
        int                                        g_saveClosing = 0;    // frames left before the dialog really closes

        // NG fix (user: "locked in place after Save Player Preset closes"): the name box still had the keyboard when
        // the window closed, so ImGui's "wants text input" stayed on (it is only recomputed in a new frame, and
        // SKSE Menu Framework stops making frames once no window is open) and the game's controls stayed blocked.
        // Closing now takes the keyboard away from the box (ClearActiveID) and keeps the window up, drawing nothing,
        // for 2 more frames so ImGui sees no text box any more; then it closes.
        void CloseSaveWindow()
        {
            ClearActiveID();
            g_saveClosing = 2;
        }

        // NG (user): the Save Player Preset dialog - a preset name, then RaceMenu's player save (LF::SavePreset on
        // the player). Enter or Save saves, Esc or Cancel closes; warns when the name is taken.
        void __stdcall RenderSaveWindow()
        {
            if (!g_saveWindow || !g_saveWindow->IsOpen.load()) {
                return;
            }
            if (g_saveClosing > 0) {
                if (--g_saveClosing == 0) {
                    SetWindowOpen(g_saveWindow, false);
                }
                return;
            }
            if (IsKeyPressed(ImGuiKey_Escape, false) && !Hotkey::Capturing()) {
                CloseSaveWindow();
                return;
            }
            SetNextWindowPos(ImVec2{ 300.0f, 200.0f }, ImGuiCond_FirstUseEver);
            if (g_saveFocus) {
                SetNextWindowFocus();
            }
            bool open = true;
            bool save = false;
            if (Begin("Save Player Preset###LFNGSavePreset", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
                Text("Name for this preset (your current look):");
                if (g_saveFocus) {
                    SetKeyboardFocusHere();
                    g_saveFocus = false;
                }
                SetNextItemWidth(320.0f);
                save = InputTextWithHint("##name", "Preset name", g_playerPresetName.data(), g_playerPresetName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                const auto      file = Files::SafeFileName(g_playerPresetName.data());
                std::error_code ec;
                if (!file.empty() && std::filesystem::exists(Files::PresetFolder() / (file + ".jslot"), ec)) {
                    TextColored(kErrorRed, "%s.jslot already exists - Save overwrites it.", file.c_str());
                }
                const bool can = !file.empty() && LF::InGame();
                BeginDisabled(!can);
                if (Button("Save", ImVec2{ 100.0f, 0.0f })) {
                    save = true;
                }
                EndDisabled();
                SameLine();
                if (Button("Cancel", ImVec2{ 100.0f, 0.0f })) {
                    open = false;
                }
                if (save && can) {
                    LF::SavePreset(0x14, g_playerPresetName.data(), true);  // 0x14 = the player
                    open = false;
                }
            }
            End();
            if (!open) {
                CloseSaveWindow();
            }
        }
    }

    namespace
    {
        SKSEMenuFramework::Model::WindowInterface* g_nearbyWindow = nullptr;  // the Nearby Actors window (AddWindow)

        // NG (user): Nearby Actors in a window of its own (its hotkey): the same section as on the Settings page
        // (scan, list, auto-ticked console selection / crosshair actor, Save Actor Preset). Opening it scans once;
        // its X or Esc closes it.
        void __stdcall RenderNearbyWindow()
        {
            if (!g_nearbyWindow || !g_nearbyWindow->IsOpen.load()) {
                return;
            }
            if (IsKeyPressed(ImGuiKey_Escape, false) && !Hotkey::Capturing()) {
                SetWindowOpen(g_nearbyWindow, false);
                return;
            }
            SetNextWindowPos(ImVec2{ 120.0f, 120.0f }, ImGuiCond_FirstUseEver);
            SetNextWindowSize(ImVec2{ 640.0f, 520.0f }, ImGuiCond_FirstUseEver);
            bool open = true;
            if (Begin("Nearby Actors###LFNGNearby", &open, ImGuiWindowFlags_NoCollapse)) {
                const bool opened = NoteDrawn(5);
                BeginPage("nearbywindow");
                if (auto snap = Ready()) {
                    if (opened) {
                        LF::ScanNearby();
                    }
                    DrawNotice();
                    DrawNearbyActors(*snap, opened);
                }
            }
            End();
            if (!open) {
                SetWindowOpen(g_nearbyWindow, false);
            }
        }
    }

    void ToggleNearbyWindow()
    {
        if (g_nearbyWindow) {
            SetWindowOpen(g_nearbyWindow, !g_nearbyWindow->IsOpen.load());
        }
    }

    void OpenSavePresetDialog()
    {
        if (g_saveWindow && !g_saveWindow->IsOpen.load()) {
            g_playerPresetName.fill('\0');
            g_saveFocus = true;
            g_saveClosing = 0;
            g_saveWindow->IsOpen.store(true);
        }
    }

    // The Open Inventory hotkey (UI task = main thread): the pick is brought up to date (UpdatePick with the live
    // console selection / last actor looked at), the Lazy Followers window closes, then LF opens the ticked actor's
    // inventory. NG (user): the Nearby Actors window stays open - tick another actor there to switch inventories.
    void OpenPickedInventory()
    {
        const auto now = LF::PickRefs();
        UpdatePick(now.console, now.crosshair, now.consoleBusy, !(g_nearbyWindow && g_nearbyWindow->IsOpen.load()));
        if (g_window) {
            SetWindowOpen(g_window, false);
        }
        LF::OpenInventory(g_pickRef);
    }

    void ToggleWindow()
    {
        if (g_window) {
            SetWindowOpen(g_window, !g_window->IsOpen.load());
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            logger::warn("SKSE Menu Framework is not installed - no menu pages");
            return;
        }
        SKSEMenuFramework::SetSection("Lazy Followers");
        SKSEMenuFramework::AddSectionItem("Followers", RenderFollowers);
        SKSEMenuFramework::AddSectionItem("Stats", RenderStats);
        SKSEMenuFramework::AddSectionItem("Spells", RenderSpells);
        SKSEMenuFramework::AddSectionItem("Combat Styles", RenderStyles);
        SKSEMenuFramework::AddSectionItem("Settings", RenderSettings);
        g_window = SKSEMenuFramework::AddWindow(RenderWindow);  // opened by the Settings hotkey
        g_saveWindow = SKSEMenuFramework::AddWindow(RenderSaveWindow);  // opened by the Save Player Preset hotkey
        g_nearbyWindow = SKSEMenuFramework::AddWindow(RenderNearbyWindow);  // opened by the Nearby Actors Window hotkey
        logger::info("Registered with SKSE Menu Framework {:.2f}", SKSEMenuFramework::GetMenuFrameworkVersion());
    }
}
