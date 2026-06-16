#include "PCH.h"

#include "Config.h"
#include "Hooks.h"
#include "Logger.h"
#include "Stats.h"

namespace {

    bool VerifyRuntime() {
        const auto rt = REL::Module::GetRuntime();
        const auto v  = REL::Module::get().version();

        // VR is explicitly out of scope for now: the AB-BA targets have
        // not been re-derived against the VR binary, so we refuse rather
        // than risk patching the wrong addresses.
        if (rt == REL::Module::Runtime::VR) {
            logs::critical(
                "Unsupported runtime: Skyrim VR ({}.{}.{}.{}). VR support "
                "is not yet implemented; the plugin will stay idle.",
                v.major(), v.minor(), v.patch(), v.build());
            return false;
        }

        // SE: the original target. Pinned to 1.5.97 -- the only SE build
        // whose addrlib ids / RVAs in this plugin were validated.
        if (rt == REL::Module::Runtime::SE) {
            if (v.major() != 1 || v.minor() != 5 || v.patch() != 97) {
                logs::critical(
                    "Unsupported SE version: {}.{}.{}.{} (this plugin pins "
                    "SE 1.5.97).",
                    v.major(), v.minor(), v.patch(), v.build());
                return false;
            }
            logs::info("Runtime confirmed: Skyrim SE {}.{}.{}.{}",
                v.major(), v.minor(), v.patch(), v.build());
            return true;
        }

        // AE: targets ported from SE via static analysis of the unpacked
        // AE 1.6.1170 binary. All cross-version addresses are resolved
        // through REL::RelocationID pairs (addrlib ids) or runtime-
        // selected RVAs/offsets, so any AE build whose Address Library
        // carries these ids works; the structural fix's VerifyCallSite()
        // is fail-safe if a call-site offset does not match.
        if (rt == REL::Module::Runtime::AE) {
            logs::info("Runtime confirmed: Skyrim AE {}.{}.{}.{}",
                v.major(), v.minor(), v.patch(), v.build());
            logs::warn(
                "AE support is newly ported (validated against 1.6.1170). "
                "Phase4Defer diagnostic logging is recommended ON for the "
                "first AE sessions; please report anomalies.");
            return true;
        }

        logs::critical("Unsupported runtime (unknown).");
        return false;
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

    // SKSE trampoline pool. Sized for Phase4Defer's two call-site
    // patches (14 bytes each via Trampoline::write_call<5>) plus
    // generous headroom. Allocating it here -- rather than inside
    // Phase4Defer::Install() -- keeps the trampoline owned by the
    // plugin's load-time prologue. The JobWaitBreaker uses safetyhook
    // (which manages its own per-hook trampoline) and does not consume
    // from this pool.
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
        "WorkerSpinLockFix loaded successfully. Phase4Defer (AB-BA "
        "spinlock prevention) + JobWaitBreaker (WaitForJobTask "
        "lost-wakeup recovery) + SiteABreaker (Site-A worker-ack "
        "deadlock recovery) active.");
    return true;
}
