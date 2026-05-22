#include "PCH.h"
#include "Hooks.h"

#include "AcquireHook.h"
#include "Breaker.h"
#include "Config.h"
#include "Phase4Defer.h"
#include "Reaper.h"
#include "WaitGraph.h"

namespace WorkerSpinLockFix::Hooks {

    bool Install() {
        const auto& cfg = Config::Get();

        WaitGraph::Init();
        Breaker::Init();

        // The reaper depends on the spin-retry address even when the
        // AcquireHook itself is disabled, so resolve it unconditionally.
        AcquireHook::ResolveSpinRetryAddress();
        AcquireHook::ResolveLockPointers();

        bool acquire_hook_active = false;
        if (cfg.acquire_hook_enabled) {
            if (AcquireHook::Install()) {
                acquire_hook_active = true;
            } else {
                logs::critical(
                    "AcquireHook installation FAILED. Cycle detection is "
                    "NOT active. The reaper backstop will still run if "
                    "enabled.");
            }
        } else {
            logs::warn(
                "AcquireHook disabled by config "
                "(acquire_hook.enabled = false). Plugin runs reaper-only.");
        }

        bool phase4_active = false;
        if (cfg.phase4_defer_enabled) {
            if (Phase4Defer::Install()) {
                phase4_active = true;
            } else {
                logs::warn(
                    "[Phase4Defer] structural fix did NOT install. "
                    "Falling back to runtime-breaker-only mode "
                    "(v1.0.0 behaviour). The runtime breaker is "
                    "sufficient for the documented engine bug; the "
                    "structural fix is a leaner mechanism but not a "
                    "prerequisite.");
            }
        } else {
            logs::info(
                "[Phase4Defer] structural fix disabled by config "
                "(phase4_defer.enabled = false). Plugin runs in "
                "v1.0.0 runtime-breaker-only mode.");
        }

        if (cfg.reaper_enabled) {
            if (!Reaper::Install()) {
                logs::warn(
                    "Reaper backstop did not start. AcquireHook is the "
                    "only active mechanism.");
            }
        } else {
            logs::info("Reaper backstop disabled by config.");
        }

        logs::info(
            "WorkerSpinLockFix armed. Surgical mode "
            "(acquire_hook_active={}, break_enabled={}, "
            "confirmation_window_ms={}, phase4_active={}, "
            "reaper_enabled={}, reaper_interval_ms={}, "
            "test_mode_enabled={}). The AcquireHook filters to "
            "LockA/LockB; all other BSSpinLocks fast-path bypass "
            "through the lock-free trampoline. When phase4_active "
            "is true the structural fix breaks the AB-BA cycle's "
            "LA->LB direction at the LockB-acquirer entry points, "
            "so the runtime breaker becomes a safety net rather "
            "than the primary mechanism.",
            acquire_hook_active,
            cfg.break_enabled,
            cfg.confirmation_window_ms,
            phase4_active,
            cfg.reaper_enabled,
            cfg.reaper_interval_ms,
            cfg.test_mode_enabled);
        return true;
    }

}
