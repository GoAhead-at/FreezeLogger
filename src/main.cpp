#include "PCH.h"

#include "AddrLib.h"
#include "Config.h"
#include "DebugTriggers.h"
#include "Logger.h"
#include "MainHook.h"
#include "PapyrusLogTap.h"
#include "RenderHook.h"
#include "SkseMessageTap.h"
#include "Symbols.h"
#include "Watchdog.h"

namespace {

    bool VerifyRuntime() {
        const auto runtime = REL::Module::get().version();
        constexpr REL::Version pinned{1, 5, 97, 0};
        if (runtime != pinned) {
            logs::error(
                "FreezeLogger pins runtime 1.5.97.0; got {}.{}.{}.{}. Refusing to install hooks.",
                runtime[0], runtime[1], runtime[2], runtime[3]);
            return false;
        }
        return true;
    }

    bool KillSwitchActive() {
        char buf[8] = {};
        std::size_t len = 0;
        if (::getenv_s(&len, buf, sizeof(buf), "FL_DISABLE") == 0 && len > 0 && buf[0] == '1') {
            logs::info("FL_DISABLE=1 — watchdog will not start.");
            return true;
        }
        return false;
    }

    void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg) {
        if (!a_msg) return;

        FreezeLogger::SkseMessageTap::OnMessage(a_msg);

        switch (a_msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            FreezeLogger::MainHook::Install();
            FreezeLogger::RenderHook::Install();
            FreezeLogger::Watchdog::Start();
            break;

        case SKSE::MessagingInterface::kDataLoaded:
            FreezeLogger::PapyrusLogTap::Install();
#if FL_DEBUG_TRIGGERS_ENABLED
            FreezeLogger::DebugTriggers::Start();
#endif
            break;

        default:
            break;
        }
    }

}

// SKSEPlugin_Version is generated automatically by add_commonlibsse_plugin
// (see __FreezeLoggerPlugin.cpp in the build dir). It uses Address Library
// runtime compatibility, which works for SE 1.5.97 (and any runtime
// covered by Address Library). The hard runtime pin to 1.5.97 lives in
// VerifyRuntime() below — we refuse to install hooks otherwise.

extern "C" __declspec(dllexport) bool SKSEPlugin_Load(const SKSE::LoadInterface* a_skse) {
    SKSE::Init(a_skse);

    FreezeLogger::Logger::Init();
    logs::info(
        "FreezeLogger v{}.{}.{} loading.",
        FL_VERSION_MAJOR, FL_VERSION_MINOR, FL_VERSION_PATCH);

    FreezeLogger::Config::Load();
    FreezeLogger::Logger::Init(FreezeLogger::Config::Get().logging.level);

    if (!VerifyRuntime()) {
        return false;
    }

    FreezeLogger::Symbols::Init();
    FreezeLogger::AddrLib::Init();

    if (KillSwitchActive()) {
        return true;  // loaded, but inert
    }

    FreezeLogger::SkseMessageTap::Install();  // registers our listener slot
    SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);

    return true;
}
