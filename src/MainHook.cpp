#include "PCH.h"
#include "MainHook.h"

#include "DebugTriggers.h"
#include "Heartbeat.h"
#include "TaskPoolBaseline.h"

namespace FreezeLogger::MainHook {

    namespace {

        // ---------------------------------------------------------------------
        // RE::Main::Update hook (Skyrim SE 1.5.97).
        //
        // We hook the CALL instruction that invokes Main::Update from inside
        // the WinMain main loop, NOT the function entry. This is the canonical
        // CommonLibSSE-NG idiom for monitoring per-frame updates.
        //
        // Verification:
        //   Cross-referenced against two independent commits of
        //   doodlum/skyrim-community-shaders (commits 08286310 and 783f5024).
        //   Both pin the same site:
        //     stl::write_thunk_call<Main_Update>(
        //       REL::RelocationID(35551, 36544).address()
        //       + REL::Relocate(0x11F, 0x160));
        //
        //   SE 1.5.97 : id 35551, offset 0x11F
        //   AE 1.6.x  : id 36544, offset 0x160
        //
        //   As of v0.5.0 we bind both via REL::RelocationID + REL::Relocate
        //   (resolved from the user's Address Library), so the hook installs
        //   on SE and AE alike. VR is not yet bound (see VerifyRuntime).
        //
        //   This NG version (3.5.5 from the colorglass registry) doesn't
        //   ship the `stl::write_thunk_call` helper, so we use the
        //   underlying trampoline.write_call<5>(...) directly.
        // ---------------------------------------------------------------------
        constexpr std::uint64_t  kCallSiteID_SE  = 35551;
        constexpr std::uint64_t  kCallSiteID_AE  = 36544;

        using UpdateFn = void(*)(RE::Main*, float);
        REL::Relocation<UpdateFn> g_originalUpdate;

        void HookedUpdate(RE::Main* a_main, float a_deltaTime) {
            Heartbeat::TickMain();

            // Periodic (≈1 Hz) capture of Skyrim's task-pool state.
            // Internally throttled — 59 of 60 calls are an atomic
            // increment+modulo only. See TaskPoolBaseline.cpp for the
            // rationale and budget. The captured baseline is rendered
            // alongside the post-freeze state in the freeze report so
            // the analyst can see exactly which slot of Singleton-B got
            // torn down.
            TaskPoolBaseline::MaybeCapture();

#if FL_DEBUG_TRIGGERS_ENABLED
            DebugTriggers::OnMainTick();
#endif

            g_originalUpdate(a_main, a_deltaTime);
        }

    }

    void Install() {
        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();

        const auto siteAddr =
            REL::RelocationID(kCallSiteID_SE, kCallSiteID_AE).address() +
            REL::Relocate(0x11F, 0x160);

        g_originalUpdate = trampoline.write_call<5>(siteAddr, HookedUpdate);

        // Arm the periodic task-pool baseline capture. Idempotent;
        // safe even if the Singleton-B anchor wasn't resolved (the capture
        // function gates on the slot being non-zero internally).
        TaskPoolBaseline::Init();

        logs::info(
            "MainHook installed at 0x{:x} (Main::Update CALL site, "
            "RelocationID {}/{}).",
            siteAddr, kCallSiteID_SE, kCallSiteID_AE);
    }

}
