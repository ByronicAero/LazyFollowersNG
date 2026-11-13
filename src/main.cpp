// main - Lazy Followers NG's entry point and SKSE message handling.

#include "Hotkey.h"
#include "LF.h"
#include "Menu.h"
#include "Voice.h"

namespace
{
    // Opens Documents/My Games/Skyrim Special Edition/SKSE/LazyFollowersNG.log (new file each launch).
    void SetupLog()
    {
        auto dir = logger::log_directory();
        if (!dir) {
            return;
        }
        // CommonLib's game-name lookup returns "Skyrim.INI" on some runtimes (1.6.1170), so pin the folder.
        auto skseDir = dir->parent_path().parent_path() / "Skyrim Special Edition" / "SKSE";
        std::error_code ec;
        std::filesystem::create_directories(skseDir, ec);
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>((skseDir / "LazyFollowersNG.log").string(), true);
        auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);
        log->set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::set_default_logger(std::move(log));
    }

    // kPostLoad = SMF pages + window and the hotkey reader (every plugin is loaded then), kPostPostLoad = the Erin race's
    // interface, kDataLoaded = LazyFollowers.esp's forms + events,
    // kPostLoadGame / kNewGame = in game (refresh the followers; a new game: Keep in New Games), kSaveGame = Keep in
    // New Games file, kPreLoadGame = leaving the current game.
    void OnMessage(SKSE::MessagingInterface::Message* a_message)
    {
        switch (a_message->type) {
        case SKSE::MessagingInterface::kPostLoad:
            LF::LoadEarlySettings();
            Menu::Register();
            Hotkey::Start();
            break;
        case SKSE::MessagingInterface::kPostPostLoad:
            LF::InitErin();  // NG (user): Erin Preset (the Erin race's extra preset)
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            LF::LoadForms();
            if (LF::FormsFound()) {
                LF::InstallSinks();
                Voice::Install();  // modded voices: follower lines in that voice
                Voice::StartOwnerScan();  // modded follower voices from the plugin files (Override box)
            }
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            LF::OnLeaveGame();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            // NG (SKSE source, Hooks_SaveLoad.cpp): the message's data is the load's result - a failed load is not
            // treated as a loaded game (no follower refresh on whatever is left in memory)
            if (reinterpret_cast<std::uintptr_t>(a_message->data) != 0) {
                LF::OnGameLoaded();
            } else {
                logger::warn("The save could not be loaded - followers not refreshed");
            }
            break;
        case SKSE::MessagingInterface::kNewGame:
            LF::OnNewGame();
            break;
        case SKSE::MessagingInterface::kSaveGame:
            LF::OnGameSaved();
            break;
        default:
            break;
        }
    }
}

// Entry point: SKSE calls this when LazyFollowersNG.dll loads; the rest waits for SKSE's messages.
SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);
    SetupLog();
    logger::info("Lazy Followers NG {} loading", SKSE::PluginDeclaration::GetSingleton()->GetVersion().string("."sv));

    LF::RegisterCosave();
    if (!SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) {
        logger::error("Could not register SKSE message listener");
        return false;
    }
    return true;
}
