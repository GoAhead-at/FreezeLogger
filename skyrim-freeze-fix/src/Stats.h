#pragma once

namespace WorkerSpinLockFix::Stats {

    // -------------------------------------------------------------------
    // v0.11 counters (Strategy F - stale-owner reaper).
    //
    //   reaped(name):  how many times the reaper has force-released the
    //                  named lock because its current holder was a thread
    //                  that no longer exists in the process.
    //   live_skips(name):
    //                  how many times the reaper noticed the lock had
    //                  been held by the same TID for longer than the
    //                  staleness threshold but the holder was still a
    //                  live thread, so we declined to act.
    //   races(name):   how many times our CAS-based force-release lost
    //                  to a concurrent engine-side modification (the
    //                  engine released or transitioned the state field
    //                  between our read and our compare-exchange). These
    //                  are NOT errors; they just mean the reaper backed
    //                  off and let the engine handle it.
    // -------------------------------------------------------------------

    void OnReaped(std::string_view which);
    void OnLiveSkip(std::string_view which);
    void OnRace(std::string_view which);

    // Starts the periodic dump thread. Idempotent.
    void StartPeriodicDump();

    // Signals the periodic dump thread to exit. Plugin lifetime is the
    // process lifetime, so this is currently called only at module unload.
    void Stop();

}
