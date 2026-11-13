// Hotkey - the Settings page's hotkeys (NG 1.1, user): "Open Window", "Summon Follower", "Summon All", "Save Player Preset",
// "Open Inventory", "Nearby Actors Window". Like Actor
// Control: the keyboard is read straight from Windows (GetAsyncKeyState) on its own thread ~100 times a
// second, so it works over any menu; a key is stored as a Windows virtual key + Ctrl / Shift / Alt. Open
// Window opens / closes the Lazy Followers window (Menu::ToggleWindow, run as a UI task); Summon Follower
// summons the follower selected in the pages (LF::SummonSelected), Summon All every follower not following
// (LF::SummonAll), Save Player Preset opens the name dialog (Menu::OpenSavePresetDialog), Open Inventory the inventory of the
// actor ticked in Settings > Nearby Actors (Menu::OpenPickedInventory), Nearby Actors Window opens / closes that window
// (Menu::ToggleNearbyWindow). One key per hotkey: binding a key takes it away from any other hotkey. Binding: a Settings key button starts a
// capture; the next key pressed (with the Ctrl / Shift / Alt held then) is bound, Esc cancels.

#include "Hotkey.h"

#include "Menu.h"

namespace Hotkey
{
    namespace
    {
        std::atomic<bool> g_capturing{ false };
        std::atomic<Key>  g_captureFor{ Key::kOpenWindow };  // which hotkey the capture binds
        std::atomic<bool> g_capturePrimed{ false };  // first read of a capture done (keys already held are ignored)

        // Skyrim is the foreground window (no hotkeys while alt-tabbed).
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

        bool IsModifierVk(int a_vk)
        {
            switch (a_vk) {
            case VK_SHIFT:
            case VK_CONTROL:
            case VK_MENU:
            case VK_LSHIFT:
            case VK_RSHIFT:
            case VK_LCONTROL:
            case VK_RCONTROL:
            case VK_LMENU:
            case VK_RMENU:
            case VK_LWIN:
            case VK_RWIN:
                return true;
            default:
                return false;
            }
        }

        // Keys that can be bound: not mouse buttons (the click that started the capture) or Ctrl / Shift /
        // Alt / Win on their own.
        bool Bindable(int a_vk)
        {
            return a_vk > VK_XBUTTON2 && a_vk != VK_ESCAPE && !IsModifierVk(a_vk) && a_vk != VK_CAPITAL && a_vk != VK_NUMLOCK && a_vk != VK_SCROLL;
        }

        // Ctrl / Shift / Alt held now (LF::kModCtrl...).
        std::uint32_t HeldMods(const std::array<bool, 256>& a_now)
        {
            return (a_now[VK_CONTROL] ? LF::kModCtrl : 0u) | (a_now[VK_SHIFT] ? LF::kModShift : 0u) | (a_now[VK_MENU] ? LF::kModAlt : 0u);
        }

        // The hotkey may fire: not while the console is open or a text box has the keyboard, and not in a
        // paused menu unless a SKSE Menu Framework window is up (then it closes / switches to ours).
        bool Allowed()
        {
            auto ui = RE::UI::GetSingleton();
            if (!ui || ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
                return false;
            }
            if (SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
                auto io = ImGuiMCP::GetIO();
                return !(io && io->WantTextInput);
            }
            return !ui->GameIsPaused();
        }

        std::array<bool, 256> g_prev{};  // poll thread only: last poll's key states

        // Every hotkey, in the Settings page's row order: its name and where it is stored.
        struct Entry
        {
            Key         key;
            const char* name;
            LF::KeyBinding (*get)();
            void (*set)(LF::KeyBinding);
        };
        constexpr Entry kEntries[] = {
            { Key::kOpenWindow, "Open Window", LF::OpenWindowKey, LF::SetOpenWindowKey },
            { Key::kSummon, "Summon Follower", LF::SummonKey, LF::SetSummonKey },
            { Key::kSummonAll, "Summon All", LF::SummonAllKey, LF::SetSummonAllKey },
            { Key::kSavePreset, "Save Player Preset", LF::SavePresetKey, LF::SetSavePresetKey },
            { Key::kOpenInventory, "Open Inventory", LF::OpenInventoryKey, LF::SetOpenInventoryKey },
            { Key::kNearbyWindow, "Nearby Actors Window", LF::NearbyWindowKey, LF::SetNearbyWindowKey },
        };

        bool Same(const LF::KeyBinding& a_a, const LF::KeyBinding& a_b) { return a_a.vk == a_b.vk && a_a.mods == a_b.mods; }

        // NG (user: "fix being able to have the same hotkey for everything"): a_key now has a_binding - every other
        // hotkey with the same key + Ctrl / Shift / Alt loses it (set to none). Returns the names cleared.
        std::string TakeKey(Key a_key, const LF::KeyBinding& a_binding)
        {
            std::string cleared;
            for (const auto& e : kEntries) {
                if (e.key != a_key && a_binding.IsSet() && Same(e.get(), a_binding)) {
                    e.set({});
                    cleared += (cleared.empty() ? "" : ", ") + std::string(e.name);
                }
            }
            return cleared;
        }

        // A binding the poll can read (set, a virtual key below 256).
        bool Usable(const LF::KeyBinding& a_b) { return a_b.IsSet() && a_b.vk < 256; }

        // One poll: binding capture while the Settings page waits for a key, else the hotkeys.
        void Poll()
        {
            if (!GameHasFocus()) {
                g_prev.fill(false);
                return;
            }
            // NG perf: every key only while picking a key; otherwise just the bound keys + Ctrl / Shift / Alt
            std::array<bool, 256> now{};
            const auto            open = LF::OpenWindowKey();
            const auto            summon = LF::SummonKey();
            const auto            summonAll = LF::SummonAllKey();
            const auto            savePreset = LF::SavePresetKey();
            const auto            openInv = LF::OpenInventoryKey();
            const auto            nearbyWin = LF::NearbyWindowKey();
            const bool            capturing = g_capturing.load();
            if (capturing) {
                for (int vk = 1; vk < 256; ++vk) {
                    now[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                }
            } else {
                if (!Usable(open) && !Usable(summon) && !Usable(summonAll) && !Usable(savePreset) && !Usable(openInv) && !Usable(nearbyWin)) {
                    g_prev.fill(false);
                    return;
                }
                for (int vk : { VK_CONTROL, VK_SHIFT, VK_MENU }) {
                    now[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                }
                for (const auto& b : { open, summon, summonAll, savePreset, openInv, nearbyWin }) {
                    if (Usable(b)) {
                        now[b.vk] = (GetAsyncKeyState(static_cast<int>(b.vk)) & 0x8000) != 0;
                    }
                }
            }
            if (capturing) {
                if (!g_capturePrimed.exchange(true)) {
                    g_prev = now;  // keys held when the button was clicked don't count
                    return;
                }
                for (int vk = 1; vk < 256; ++vk) {
                    if (!now[vk] || g_prev[vk]) {
                        continue;
                    }
                    if (vk == VK_ESCAPE) {
                        g_capturing = false;
                        break;
                    }
                    if (Bindable(vk)) {
                        const LF::KeyBinding k{ static_cast<std::uint32_t>(vk), HeldMods(now) };
                        for (const auto& e : kEntries) {
                            if (e.key == g_captureFor.load()) {
                                const auto cleared = TakeKey(e.key, k);  // the key leaves any other hotkey first
                                e.set(k);
                                logger::info("{} hotkey set to {}{}", e.name, Name(k), cleared.empty() ? "" : " (taken from " + cleared + ")");
                                if (!cleared.empty()) {
                                    LF::SetNotice(std::format("{} now uses {} - removed from {}.", e.name, Name(k), cleared));
                                }
                            }
                        }
                        g_capturing = false;
                        break;
                    }
                }
                g_prev = now;
                if (!g_capturing.load()) {
                    g_prev.fill(true);  // capture over: the key just bound must be released before it can fire
                }
                return;
            }
            auto pressed = [&](const LF::KeyBinding& a_b) { return Usable(a_b) && now[a_b.vk] && !g_prev[a_b.vk] && HeldMods(now) == a_b.mods; };
            const bool openPressed = pressed(open);
            const bool summonPressed = pressed(summon);
            const bool summonAllPressed = pressed(summonAll);
            const bool savePresetPressed = pressed(savePreset);
            const bool openInvPressed = pressed(openInv);
            const bool nearbyWinPressed = pressed(nearbyWin);
            g_prev = now;
            if ((openPressed || summonPressed || summonAllPressed || savePresetPressed || openInvPressed || nearbyWinPressed) && Allowed()) {
                if (openPressed) {
                    SKSE::GetTaskInterface()->AddUITask([]() { Menu::ToggleWindow(); });
                }
                if (summonPressed) {
                    LF::SummonSelected();
                }
                if (summonAllPressed) {
                    LF::SummonAll(true);
                }
                if (savePresetPressed) {
                    SKSE::GetTaskInterface()->AddUITask([]() { Menu::OpenSavePresetDialog(); });
                }
                if (openInvPressed) {
                    SKSE::GetTaskInterface()->AddUITask([]() { Menu::OpenPickedInventory(); });
                }
                if (nearbyWinPressed) {
                    SKSE::GetTaskInterface()->AddUITask([]() { Menu::ToggleNearbyWindow(); });
                }
            }
        }
    }

    void Start()
    {
        static std::atomic<bool> started{ false };
        if (started.exchange(true)) {
            return;
        }
        std::thread([]() {
            for (;;) {
                std::this_thread::sleep_for(10ms);
                try {
                    Poll();
                } catch (...) {
                }
            }
        }).detach();
        logger::info("Hotkey reader started (Open Window: {}, Summon Follower: {}, Summon All: {}, Save Player Preset: {}, Open Inventory: {}, Nearby Actors Window: {})",
            Name(LF::OpenWindowKey()), Name(LF::SummonKey()), Name(LF::SummonAllKey()), Name(LF::SavePresetKey()), Name(LF::OpenInventoryKey()),
            Name(LF::NearbyWindowKey()));
    }

    // NG (user): a settings file from before "one key per hotkey" can give one key to several hotkeys - the first row
    // keeps it. Called by the Settings page's Hotkey section once a game is loaded (saving the settings file before
    // that would write the game settings' defaults).
    void FixShared()
    {
        for (const auto& e : kEntries) {
            if (const auto cleared = TakeKey(e.key, e.get()); !cleared.empty()) {
                logger::info("{} was also on {} - kept on {} only", Name(e.get()), cleared, e.name);
            }
        }
    }

    void BeginCapture(Key a_key)
    {
        g_captureFor = a_key;
        g_capturePrimed = false;
        g_capturing = true;
    }

    void CancelCapture() { g_capturing = false; }

    bool Capturing() { return g_capturing.load(); }

    bool Capturing(Key a_key) { return g_capturing.load() && g_captureFor.load() == a_key; }

    // The key's name from Windows (its scan code's key name), with Ctrl / Shift / Alt in front.
    std::string Name(const LF::KeyBinding& a_binding)
    {
        if (!a_binding.IsSet()) {
            return "(none)";
        }
        std::string out;
        if (a_binding.mods & LF::kModCtrl) {
            out += "Ctrl + ";
        }
        if (a_binding.mods & LF::kModShift) {
            out += "Shift + ";
        }
        if (a_binding.mods & LF::kModAlt) {
            out += "Alt + ";
        }
        LONG scan = static_cast<LONG>(MapVirtualKeyA(a_binding.vk, MAPVK_VK_TO_VSC));
        switch (a_binding.vk) {  // keys whose scan code needs the "extended" bit for the right name
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_DIVIDE:
        case VK_APPS:
            scan |= 0x100;
            break;
        default:
            break;
        }
        char name[64]{};
        if (scan && GetKeyNameTextA(scan << 16, name, sizeof(name)) > 0) {
            out += name;
        } else {
            out += std::format("Key 0x{:02X}", a_binding.vk);
        }
        return out;
    }
}
