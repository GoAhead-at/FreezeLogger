#include "PCH.h"
#include "Hooks.h"

#include "Config.h"
#include "JobWaitBreaker.h"
#include "Phase4Defer.h"
#include "SkyrimAnchors.h"

namespace WorkerSpinLockFix::Hooks {

    bool Install() {
        const auto& cfg = Config::Get();

        // Resolve the WaitForJobTask / Singleton-B anchors up-front (version-
        // independent signature scan). The JobWaitBreaker gates on this.
        SkyrimAnchors::Init();

        // ---- Layer 1: structural AB-BA prevention (Phase4Defer) ----------
        bool phase4_active = false;
        if (cfg.phase4_defer_enabled) {
            if (Phase4Defer::Install()) {
                phase4_active = true;
            } else {
                logs::warn(
                    "[Phase4Defer] structural fix did NOT install. The AB-BA "
                    "spinlock prevention layer is inactive this session; the "
                    "engine runs unmodified for that bug.");
            }
        } else {
            logs::info(
                "[Phase4Defer] structural fix disabled by config "
                "(phase4_defer.enabled = false).");
        }

        // ---- Layer 2: WaitForJobTask lost-wakeup recovery ---------------
        bool jwb_active = false;
        if (cfg.jwb_enabled) {
            if (JobWaitBreaker::Install()) {
                jwb_active = true;
            }
        } else {
            logs::info(
                "[JobWaitBreaker] disabled by config "
                "(job_wait_breaker.enabled = false).");
        }

        logs::info(
            "WorkerSpinLockFix armed. phase4_active={} (AB-BA spinlock "
            "prevention), job_wait_breaker_active={} (WaitForJobTask "
            "lost-wakeup recovery, detect_only={}).",
            phase4_active, jwb_active, cfg.jwb_detect_only);

        return phase4_active || jwb_active;
    }

}
