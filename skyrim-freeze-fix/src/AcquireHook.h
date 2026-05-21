#pragma once

#include "WaitGraph.h"

namespace WorkerSpinLockFix::AcquireHook {

    // Installs an entry-point inline hook on BSSpinLock::Acquire (id 12210)
    // using safetyhook::create_inline. The detour does a surgical filter:
    // only the two engine BSSpinLocks that participate in the documented
    // AB-BA deadlock (LockA at SkyrimSE+0x2eff8e0, LockB at +0x2f3b8e8)
    // do real work; every other BSSpinLock pays only one pointer compare
    // and a tail-call. Cost on the uncontended common path is ~2 ns.
    //
    // Returns true on success.
    bool Install();

    // Resolves the spin-retry RVA from id 12210 + 0x8a. Idempotent. Cached
    // on first success. Returns 0 on failure. The reaper depends on this
    // (it uses the address as a stack-walk pattern) so it must succeed
    // even when AcquireHook itself is disabled or fails to install.
    std::uintptr_t ResolveSpinRetryAddress() noexcept;

    // Spin-retry RVA, exposed for the stale-owner reaper (Reaper.cpp).
    // Returns 0 if neither Install() nor ResolveSpinRetryAddress() has
    // succeeded yet.
    std::uintptr_t SpinRetryAddress() noexcept;

    // The two BSSpinLock pointers the surgical filter watches. Resolved
    // by Install() or by ResolveLockPointers(). Return nullptr if the
    // module base could not be resolved.
    WaitGraph::Lock* LockA() noexcept;
    WaitGraph::Lock* LockB() noexcept;

    // Idempotent. Resolves LockA / LockB from the SkyrimSE.exe base + the
    // hard-coded RVAs. Called automatically by Install(), exposed
    // separately so the reaper has access even when the hook is disabled.
    void ResolveLockPointers() noexcept;

    // Extends the surgical filter to also consider the two pointers `a`
    // and `b` as "interesting" locks that flow through the slow path
    // (cycle detection + breaker). Used ONLY by the optional test-mode
    // module (TestMode.cpp) to validate the breaker against a synthetic
    // AB-BA without touching the real engine LockA / LockB.
    //
    // Pass nullptr to clear the test slots (test mode finished).
    //
    // Caller must guarantee the pointed-to BSSpinLock objects outlive
    // any thread that may call BSSpinLock::Acquire on them. The hook's
    // hot path reads these pointers without synchronization (one-shot
    // write, racy read across threads) which is safe because the test
    // module installs them once before spawning the test threads and
    // never relocates them.
    void AddTestLocks(WaitGraph::Lock* a, WaitGraph::Lock* b) noexcept;

}
