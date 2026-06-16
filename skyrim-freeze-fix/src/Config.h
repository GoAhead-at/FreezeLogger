#pragma once

namespace WorkerSpinLockFix::Config {

    struct Settings {
        bool enabled{ true };

        std::uint32_t stats_interval_s{ 60 };

        // ---- Phase 4 structural defer (Phase4Defer.cpp) --------------------

        // Phase 4 layered structural fix on top of the v1.0.0 runtime
        // breaker. Wraps id 19369 (the LockA acquirer) and gates id
        // 40333 / id 40334 (the two LockB acquirers) so that calls
        // into the LockB acquirers are deferred when the current
        // thread is inside the LockA acquirer. This breaks the LA->LB
        // edge of the AB-BA cycle structurally; the runtime breaker
        // remains installed as defence-in-depth.
        //
        // See docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md
        // for the full design and correctness audit.
        //
        // Default ON: the structural fix is the cleaner mechanism;
        // the runtime breaker becomes the safety net rather than the
        // primary detection layer. Set to false to revert to the
        // v1.0.0 runtime-breaker-only configuration without
        // rebuilding or removing the DLL.
        bool phase4_defer_enabled{ true };

        // Per-call diagnostic logging for Phase4Defer. When true, every
        // entry into the LockA wrap and every entry into the two LockB
        // gates is logged with its arguments, the captured return
        // value, and the deferred-queue depth. Volume is high during
        // gameplay (hundreds of lines per second) but the cost is
        // bounded -- spdlog buffers writes asynchronously. Intended
        // for diagnosing regressions in scripted-animation activators
        // (skyshards, idle pickups, etc.) where the behavioural
        // symptom doesn't pinpoint which hook is at fault.
        //
        // Default OFF. Enable, reproduce the symptom once, disable
        // again. The log will show whether the hooks were even
        // entered for the affected interaction, and what values
        // flowed through them.
        bool phase4_defer_diagnostic_logging{ false };

        // ---- JobWaitBreaker (WaitForJobTask lost-wakeup recovery) ----------
        //
        // Recovers the engine from the Skyrim main-thread WaitForJobTask
        // lost-wakeup hang (case-study 27/28): main parks on a job
        // completion event that a producer tore down without signalling,
        // so the game freezes. See JobWaitBreaker.h for the mechanism.

        // Master switch for the module.
        bool jwb_enabled{ true };

        // Detect-only (default): when the lost-wakeup signature is seen,
        // log it but do NOT alter engine behaviour. Set to false to
        // actually release main (SetEvent on the parked job handle).
        // Ships true until the release path is field-validated.
        bool jwb_detect_only{ true };

        // How long main must be parked in WaitForJobTask before the
        // module considers it stuck. Far above any legitimate job-wait
        // during play, well below the FreezeLogger 15 s watchdog.
        std::uint32_t jwb_dwell_threshold_ms{ 5000 };

        // Watchdog poll cadence.
        std::uint32_t jwb_poll_interval_ms{ 1000 };

        // Zero-progress confirmation window. After the dwell threshold is
        // reached the watchdog snapshots the job entry, waits this long,
        // and re-samples; it only acts if the job made NO progress across
        // the window (chain still null, or entry bytes byte-for-byte
        // identical). A long-but-progressing job mutates its counters
        // within this window and is left alone, so raising this value
        // makes the breaker more conservative.
        std::uint32_t jwb_recheck_window_ms{ 1500 };

        // Verbose per-decision logging for the watchdog.
        bool jwb_diagnostic_logging{ false };

        // ---- SiteABreaker (Site-A worker-ack deadlock recovery) ------------
        //
        // Recovers the engine from the Skyrim main-thread Site-A worker-ack
        // deadlock (case-study 29): Main::Update parks in id 34554 on a
        // worker-ack event that a worker consumed-the-wake-but-never-signaled,
        // so the game freezes. Distinct from the WaitForJobTask hang above.
        // See SiteABreaker.h for the mechanism.

        // Master switch for the module.
        bool sab_enabled{ true };

        // Detect-only (default): when the deadlock signature is seen, log it
        // but do NOT alter engine behaviour. Set to false to actually release
        // main (SetEvent on the captured worker-ack handle). Ships true until
        // the release path is field-validated.
        bool sab_detect_only{ true };

        // How long main must be parked in id 34554 before the module
        // considers it stuck. Far above any legitimate Site-A wait during
        // play, well below the FreezeLogger 15 s watchdog.
        std::uint32_t sab_dwell_threshold_ms{ 5000 };

        // Watchdog poll cadence.
        std::uint32_t sab_poll_interval_ms{ 1000 };

        // Zero-progress confirmation window. After the dwell threshold the
        // watchdog samples Singleton-A, waits this long, and re-samples; it
        // only acts if main stayed in the SAME Site-A episode (still parked,
        // pending=1, work-id + ack handle unchanged) across the window. A
        // wait that is going to complete clears pending inside this window
        // and is left alone, so raising this value is more conservative.
        std::uint32_t sab_recheck_window_ms{ 1500 };

        // Verbose per-decision logging for the watchdog.
        bool sab_diagnostic_logging{ false };
    };

    void Init();
    const Settings& Get();

    std::filesystem::path ConfigPath();

}
