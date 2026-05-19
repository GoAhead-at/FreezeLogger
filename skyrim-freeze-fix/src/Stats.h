#pragma once

namespace WorkerSpinLockFix::Stats {

    // -------------------------------------------------------------------
    // v0.13 counters (extends v0.12 with passthrough).
    //
    // The plugin runs two layers concurrently:
    //
    //   - Approach G (structural): one std::recursive_mutex shared
    //     between the call-site hooks of id 19369 and id 40706.
    //     `OnEntry(name)` is called on every (top-level) entry into
    //     either function; `OnRecursiveEntry(name)` is called when a
    //     thread already holds the mutex and re-enters (recursive
    //     id 19369, or id 40706 -> id 19369 chain). `OnContended()`
    //     is called when a thread had to wait for the mutex.
    //   - Approach F (reaper, safety net): same as v0.11. `OnReaped`,
    //     `OnLiveSkip`, `OnRace` semantics unchanged.
    //
    // The two are reported separately each interval so we can verify
    // that Approach G is doing the work (entry counts grow, mutex
    // gets contended occasionally) and that the reaper stays at zero
    // (no phantom-owner conditions because Approach G prevents the
    // setup that produces them).
    // -------------------------------------------------------------------

    void OnEntry(std::string_view which);              // "id_19369" | "id_40706"
    void OnRecursiveEntry(std::string_view which);
    void OnContended(std::string_view which);

    // v0.13: id_40706 invocations whose this+0x150 != &LockB. These
    // are per-object instances, not on the AB-BA path; we let them
    // through without taking our mutex. Counted separately so the
    // ratio of serialised vs pass-through entries is visible.
    void OnPassthrough(std::string_view which);

    void OnReaped(std::string_view which);             // "LockA" | "LockB"
    void OnLiveSkip(std::string_view which);
    void OnRace(std::string_view which);

    // Starts the periodic dump thread. Idempotent.
    void StartPeriodicDump();

    // Signals the periodic dump thread to exit. Plugin lifetime is the
    // process lifetime, so this is currently called only at module unload.
    void Stop();

}
