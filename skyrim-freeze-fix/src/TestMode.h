#pragma once

namespace WorkerSpinLockFix::TestMode {

    // Synthetic AB-BA validation harness.
    //
    // When invoked, allocates two heap-resident BSSpinLock-shaped objects,
    // registers them with AcquireHook::AddTestLocks so they flow through
    // the surgical filter, and spawns two worker threads that deliberately
    // construct an AB-BA cycle by calling the real BSSpinLock::Acquire
    // (id 12210) on the test locks in opposite orders. The test verifies
    // that the time-based confirmation flow detects the cycle, confirms
    // it after the configured window, force-releases one test lock, and
    // both test threads complete instead of spinning forever.
    //
    // Run() is asynchronous: it spawns a coordinator thread and returns
    // immediately. The coordinator logs a [TEST] SUCCESS or [TEST] FAILURE
    // line within ~`timeout_ms` of being invoked. Safety net: if the
    // breaker fails to fire, the coordinator manually clears the test
    // locks' state field after the timeout so the test threads do not
    // remain spinning forever.
    //
    // Idempotent: a second Run() call while the first is still active is
    // a no-op (returns false).
    bool Run(std::uint32_t timeout_ms = 10000);

}
