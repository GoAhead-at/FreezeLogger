#include "PCH.h"

#include "AddrLib.h"
#include "Config.h"
#include "DebugTriggers.h"
#include "Logger.h"
#include "MainHook.h"
#include "PapyrusLogTap.h"
#include "RenderHook.h"
#include "SkseMessageTap.h"
#include "SkyrimAnchors.h"
#include "Symbols.h"
#include "Watchdog.h"

namespace {

    // Multi-runtime support (v0.5.0): we no longer pin a single build.
    // The generic diagnostics (heartbeat, watchdog, threads, modules,
    // Papyrus, anim-graph, mini-dump) work on any CommonLibSSE-NG-supported
    // runtime; the deep Site-B / WaitForJobTask probe is anchored at runtime
    // by signature (see SkyrimAnchors) rather than by per-version RVAs.
    //
    // We accept:
    //   - SE  >= 1.5.97  (the 1.5.x branch we were built and validated on)
    //   - AE  (1.6.x, any point release)
    // VR is recognised but DEFERRED: the Main::Update / Init_InitD3D hooks
    // bind SE+AE RelocationIDs only, so installing on VR would throw. We
    // refuse it explicitly with a clear message until VR IDs are added.
    // Anything older than SE 1.5.97 on the 1.5 branch is refused (the hook
    // call-site IDs and struct assumptions predate it).
    bool VerifyRuntime() {
        const auto runtime = REL::Module::get().version();

        if (REL::Module::IsVR()) {
            logs::error(
                "FreezeLogger: VR runtime {}.{}.{}.{} detected. VR support is "
                "not yet wired (hooks bind SE/AE only); refusing to install to "
                "avoid a crash. VR is planned — see CHANGELOG.",
                runtime[0], runtime[1], runtime[2], runtime[3]);
            return false;
        }

        if (REL::Module::IsAE()) {
            logs::info(
                "FreezeLogger: runtime {}.{}.{}.{} (AE) accepted.",
                runtime[0], runtime[1], runtime[2], runtime[3]);
            return true;
        }

        // SE branch: require >= 1.5.97.
        constexpr REL::Version kMinSE{1, 5, 97, 0};
        if (runtime < kMinSE) {
            logs::error(
                "FreezeLogger requires SE >= 1.5.97 or AE; got {}.{}.{}.{}. "
                "Refusing to install hooks.",
                runtime[0], runtime[1], runtime[2], runtime[3]);
            return false;
        }
        logs::info(
            "FreezeLogger: runtime {}.{}.{}.{} (SE) accepted.",
            runtime[0], runtime[1], runtime[2], runtime[3]);
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
    FreezeLogger::SkyrimAnchors::Init();

    if (KillSwitchActive()) {
        return true;  // loaded, but inert
    }

    FreezeLogger::SkseMessageTap::Install();  // registers our listener slot
    SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);

    return true;
}
