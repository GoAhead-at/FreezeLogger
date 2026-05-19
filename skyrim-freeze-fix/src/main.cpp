#include "PCH.h"

#include "Config.h"
#include "Hooks.h"
#include "Logger.h"
#include "Stats.h"

namespace {

    bool VerifyRuntime() {
        const auto rt = REL::Module::GetRuntime();
        if (rt != REL::Module::Runtime::SE) {
            logs::critical("Unsupported runtime: only Skyrim SE 1.5.97 is targeted.");
            return false;
        }

        const auto v = REL::Module::get().version();
        if (v.major() != 1 || v.minor() != 5 || v.patch() != 97) {
            logs::critical(
                "Unsupported SE version: {}.{}.{}.{} (this plugin pins SE 1.5.97).",
                v.major(), v.minor(), v.patch(), v.build());
            return false;
        }

        logs::info("Runtime confirmed: Skyrim SE {}.{}.{}.{}",
            v.major(), v.minor(), v.patch(), v.build());
        return true;
    }

    void OnSkseMessage(SKSE::MessagingInterface::Message* msg) {
        if (msg == nullptr) {
            return;
        }
        switch (msg->type) {
        case SKSE::MessagingInterface::kPostLoad:
            logs::info("SKSE post-load message received.");
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            logs::info("Data files loaded; plugin remains active.");
            break;
        default:
            break;
        }
    }

}

extern "C" [[maybe_unused]] __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(
    const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    WorkerSpinLockFix::Logger::Init();

    logs::info("=========================================================");
    logs::info("WorkerSpinLockFix v{}.{}.{} loading.",
        WSLF_VERSION_MAJOR, WSLF_VERSION_MINOR, WSLF_VERSION_PATCH);
    logs::info("=========================================================");

    if (!VerifyRuntime()) {
        logs::critical("Plugin will not install hooks. Game will continue normally.");
        return true;  // do not abort the game; just stay idle
    }

    WorkerSpinLockFix::Config::Init();

    if (!WorkerSpinLockFix::Config::Get().enabled) {
        logs::warn("Plugin is disabled by config (plugin.enabled = false). Hooks NOT installed.");
        return true;
    }

    if (!WorkerSpinLockFix::Hooks::Install()) {
        logs::critical("Hook installation failed; plugin will run idle.");
        return true;
    }

    WorkerSpinLockFix::Stats::StartPeriodicDump();

    if (auto* msg = SKSE::GetMessagingInterface(); msg != nullptr) {
        msg->RegisterListener(OnSkseMessage);
    }

    logs::info("WorkerSpinLockFix loaded successfully. Hooks active.");
    return true;
}
