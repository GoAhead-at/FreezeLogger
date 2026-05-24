#include "PCH.h"

#include "Config.h"
#include "Hooks.h"
#include "Logger.h"
#include "Stats.h"
#include "TestMode.h"

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
            if (WorkerSpinLockFix::Config::Get().test_mode_enabled) {
                logs::warn(
                    "[TEST] test_mode is ENABLED in config. Launching "
                    "synthetic AB-BA validation harness. Disable "
                    "test_mode in WorkerSpinLockFix.toml for normal play.");
                WorkerSpinLockFix::TestMode::Run();
            }
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

    // SKSE trampoline pool. Sized for Phase4Defer's two call-site
    // patches (14 bytes each via Trampoline::write_call<5>) plus
    // generous headroom for any future patches. Allocating it here
    // -- rather than inside Phase4Defer::Install() -- keeps the
    // trampoline owned by the plugin's load-time prologue and avoids
    // re-allocating across hook reinstalls. AcquireHook continues to
    // use safetyhook (which manages its own per-hook trampoline) and
    // does not consume from this pool.
    SKSE::AllocTrampoline(64);

    if (!WorkerSpinLockFix::Hooks::Install()) {
        logs::critical("Hook installation failed; plugin will run idle.");
        return true;
    }

    WorkerSpinLockFix::Stats::StartPeriodicDump();

    if (auto* msg = SKSE::GetMessagingInterface(); msg != nullptr) {
        msg->RegisterListener(OnSkseMessage);
    }

    logs::info(
        "WorkerSpinLockFix loaded successfully. Surgical AcquireHook "
        "(LockA/LockB only) + WaitGraph + Breaker + Reaper backstop "
        "active.");
    return true;
}
