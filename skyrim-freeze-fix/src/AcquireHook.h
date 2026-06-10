#pragma once

#include "WaitGraph.h"

namespace WorkerSpinLockFix::AcquireHook {

    // Installs a MID-function hook at BSSpinLock::Acquire +0x8a (id 12210)
    // using safetyhook::create_mid. +0x8a is the retry point inside the
    // engine's OUTER backoff spin loop, reached only by a thread that has
    // already exhausted the inner pause spin -- i.e. one that is genuinely
    // stuck. The callback does a surgical filter: only the two engine
    // BSSpinLocks that participate in the documented AB-BA deadlock (LockA
    // at SkyrimSE+0x2eff8e0, LockB at +0x2f3b8e8) drive cycle detection.
    //
    // Unlike the previous entry-point inline hook, uncontended and
    // recursive acquires -- the overwhelming majority -- never reach the
    // hook and run entirely native (zero added per-acquire cost). Only the
    // backoff loop of an actually-contended lock pays anything.
    //
    // Returns true on success.
    bool Install();

    // Resolves the spin-retry RVA from id 12210 + 0x8a (the mid-hook
    // target). Idempotent. Cached on first success. Returns 0 on failure.
    std::uintptr_t ResolveSpinRetryAddress() noexcept;

    // Spin-retry RVA. Returns 0 if neither Install() nor
    // ResolveSpinRetryAddress() has succeeded yet.
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
