#pragma once

#include "Watchdog.h"

#include <cstdint>

namespace FreezeLogger::Reporter {

    // Captures every enabled snapshot section and writes a freeze report
    // to the configured output directory. Called from the watchdog thread.
    void CaptureAndWrite(
        Watchdog::StalledThread a_stalledThread,
        std::uint64_t           a_mainAgeMs,
        std::uint64_t           a_renderAgeMs);

    // On-demand capture triggered by the test_mode hotkey. Writes the same
    // report shape as CaptureAndWrite but flags it as a MANUAL capture in
    // the header and filename — the game was not necessarily frozen. Safe
    // to call from any thread; serialized with CaptureAndWrite internally.
    void CaptureManual();

    // Appends a "Resolved at T+Xs" annotation to the most recent report.
    // Safe to call even if no report exists (logs and returns).
    void AnnotateLatestResolved(std::uint64_t a_resolvedAfterMs);

    // Drops the oldest report files when the count exceeds keep_last_n.
    void EnforceRetention();

}
