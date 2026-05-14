#pragma once

#include <cstdint>

namespace FreezeLogger::Watchdog {

    enum class StalledThread : std::uint8_t {
        None   = 0,
        Main   = 1,
        Render = 2,
        Both   = 3,
    };

    inline constexpr const char* ToString(StalledThread t) {
        switch (t) {
        case StalledThread::Main:   return "main";
        case StalledThread::Render: return "render";
        case StalledThread::Both:   return "both";
        default:                    return "none";
        }
    }

    void Start();
    void Stop();

    // Pure-function classifier (header-inline so unit tests can link without
    // dragging in PCH/SKSE). A heartbeat of 0 means "not seeded yet".
    inline StalledThread Classify(
        std::uint64_t a_now,
        std::uint64_t a_mainHeartbeat,
        std::uint64_t a_renderHeartbeat,
        std::uint32_t a_thresholdMs) noexcept
    {
        const auto age = [&](std::uint64_t hb) -> std::uint64_t {
            if (hb == 0)        return 0;
            if (a_now <= hb)    return 0;
            return a_now - hb;
        };
        const bool mainStalled =
            a_mainHeartbeat   != 0 && age(a_mainHeartbeat)   > a_thresholdMs;
        const bool renderStalled =
            a_renderHeartbeat != 0 && age(a_renderHeartbeat) > a_thresholdMs;

        if (mainStalled && renderStalled) return StalledThread::Both;
        if (mainStalled)                  return StalledThread::Main;
        if (renderStalled)                return StalledThread::Render;
        return StalledThread::None;
    }

    // ---------------------------------------------------------------------
    // Pure state machine for the watchdog. Exposed in the header so unit
    // tests can drive it with a virtual clock without linking the SKSE
    // runtime. The real Loop() in Watchdog.cpp is just `sleep + Step + act`.
    // ---------------------------------------------------------------------

    enum class Action : std::uint8_t {
        None            = 0,
        EmitSnapshot    = 1,
        AnnotateResolve = 2,
        // Stall would have tripped a snapshot, but the caller passed
        // a_canTrip=false (e.g. Skyrim window not foreground). Loop logs
        // it; no snapshot is written. Latches into State::suppressed_freeze
        // so the loop only logs once per stall, not every check_interval.
        SuppressedStall = 3,
    };

    struct State {
        std::uint64_t   last_snapshot_at_ms  = 0;
        // True after we've written a snapshot and are waiting for resolution.
        bool            in_freeze            = false;
        // True after a stall was observed but suppressed (no snapshot written).
        // Mutually exclusive with in_freeze.
        bool            suppressed_freeze    = false;
        std::uint64_t   freeze_started_at_ms = 0;
        StalledThread   freeze_thread        = StalledThread::None;
    };

    struct StepResult {
        Action          action            = Action::None;
        StalledThread   stalled           = StalledThread::None;  // valid for EmitSnapshot / SuppressedStall
        std::uint64_t   resolved_after_ms = 0;                    // valid for AnnotateResolve
        std::uint64_t   main_age_ms       = 0;                    // for the report header
        std::uint64_t   render_age_ms     = 0;
    };

    // a_canTrip = false means: even if heartbeats look stalled, do not write
    // a snapshot. Callers use this to suppress false-positives when the
    // Skyrim window is not in the foreground (Skyrim's WinMain idle path
    // intentionally sleeps the main thread when unfocused).
    inline StepResult Step(
        State&        a_state,
        std::uint64_t a_now,
        std::uint64_t a_mainHeartbeat,
        std::uint64_t a_renderHeartbeat,
        std::uint32_t a_thresholdMs,
        std::uint32_t a_cooldownSeconds,
        bool          a_canTrip = true) noexcept
    {
        StepResult result{};

        // Don't trip until at least one heartbeat has been seeded.
        if (a_mainHeartbeat == 0 && a_renderHeartbeat == 0) {
            return result;
        }

        const auto age = [&](std::uint64_t hb) -> std::uint64_t {
            if (hb == 0)         return 0;
            if (a_now <= hb)     return 0;
            return a_now - hb;
        };
        result.main_age_ms   = age(a_mainHeartbeat);
        result.render_age_ms = age(a_renderHeartbeat);

        const auto stalled = Classify(a_now, a_mainHeartbeat, a_renderHeartbeat, a_thresholdMs);

        // Fresh stall observed and we're not already tracking one.
        if (stalled != StalledThread::None &&
            !a_state.in_freeze &&
            !a_state.suppressed_freeze)
        {
            if (!a_canTrip) {
                // Suppression path: latch into suppressed_freeze so we don't
                // re-classify (and re-log) every check_interval. No snapshot,
                // no last_snapshot_at_ms update, no cooldown gating.
                a_state.suppressed_freeze    = true;
                a_state.freeze_thread        = stalled;
                a_state.freeze_started_at_ms = a_now;

                result.action  = Action::SuppressedStall;
                result.stalled = stalled;
                return result;
            }

            const bool inCooldown =
                a_state.last_snapshot_at_ms != 0 &&
                (a_now - a_state.last_snapshot_at_ms) <
                    static_cast<std::uint64_t>(a_cooldownSeconds) * 1000ULL;
            if (inCooldown) {
                return result;
            }
            a_state.in_freeze            = true;
            a_state.freeze_started_at_ms = a_now;
            a_state.freeze_thread        = stalled;
            a_state.last_snapshot_at_ms  = a_now;

            result.action  = Action::EmitSnapshot;
            result.stalled = stalled;
            return result;
        }

        // Stall resolved.
        if (stalled == StalledThread::None) {
            if (a_state.in_freeze) {
                result.action            = Action::AnnotateResolve;
                result.resolved_after_ms = a_now - a_state.freeze_started_at_ms;
                a_state.in_freeze        = false;
                a_state.freeze_thread    = StalledThread::None;
                return result;
            }
            if (a_state.suppressed_freeze) {
                // Silently clear: we never emitted a report to annotate.
                a_state.suppressed_freeze    = false;
                a_state.freeze_thread        = StalledThread::None;
                a_state.freeze_started_at_ms = 0;
                return result;
            }
        }

        return result;
    }

}
