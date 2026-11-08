// Hotkey - the "Open Window" hotkey of the Settings page (NG 1.1, user). Like Actor Control: the keyboard is
// read straight from Windows (GetAsyncKeyState) on its own thread ~100 times a second, so it works over any
// menu; the key is stored as a Windows virtual key + Ctrl / Shift / Alt. Pressing it opens / closes the
// Lazy Followers window (Menu::ToggleWindow, run as a UI task). Binding: the Settings page's key button
// starts a capture; the next key pressed (with the Ctrl / Shift / Alt held then) is bound, Esc cancels.

#include "Hotkey.h"

#include "Menu.h"

namespace Hotkey
{
    namespace
    {
        std::atomic<bool> g_capturing{ false };
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

        // One poll: binding capture while the Settings page waits for a key, else the hotkey.
        void Poll()
        {
            if (!GameHasFocus()) {
                g_prev.fill(false);
                return;
            }
            // NG perf: every key only while picking a key; otherwise just the bound key + Ctrl / Shift / Alt
            std::array<bool, 256> now{};
            const auto            b = LF::OpenWindowKey();
            const bool            capturing = g_capturing.load();
            if (capturing) {
                for (int vk = 1; vk < 256; ++vk) {
                    now[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
                }
            } else {
                if (!b.IsSet() || b.vk >= 256) {
                    g_prev.fill(false);
                    return;
                }
                for (int vk : { static_cast<int>(b.vk), VK_CONTROL, VK_SHIFT, VK_MENU }) {
                    now[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
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
                        LF::SetOpenWindowKey({ static_cast<std::uint32_t>(vk), HeldMods(now) });
                        logger::info("Open Window hotkey set to {}", Name(LF::OpenWindowKey()));
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
            const bool pressed = now[b.vk] && !g_prev[b.vk] && HeldMods(now) == b.mods;
            g_prev = now;
            if (pressed && Allowed()) {
                SKSE::GetTaskInterface()->AddUITask([]() { Menu::ToggleWindow(); });
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
        logger::info("Hotkey reader started (Open Window: {})", Name(LF::OpenWindowKey()));
    }

    void BeginCapture()
    {
        g_capturePrimed = false;
        g_capturing = true;
    }

    void CancelCapture() { g_capturing = false; }

    bool Capturing() { return g_capturing.load(); }

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
