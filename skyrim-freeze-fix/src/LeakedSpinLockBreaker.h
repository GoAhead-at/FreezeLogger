#pragma once

namespace WorkerSpinLockFix::LeakedSpinLockBreaker {

    // Layer 5: recovery for the LEAKED BSSpinLock hard-freeze (case-study
    // 30, first proven in freeze_2026-06-25_222116 with FreezeLogger
    // v0.11.1's parked-holder steady-state probe).
    //
    // The signature, isolated across four captures:
    //   * A thread (the victim -- main, reached via the Faster HDT-SMP
    //     cloth chain id 35565 -> BSSpinLock::Acquire id 12210) spins
    //     forever on a heap BSSpinLock.
    //   * That lock's [+0] owner field names a DIFFERENT thread that is
    //     parked in a kernel wait inside the engine's idle worker pool
    //     (id 68058, waiting on the pool Semaphore) -- i.e. it acquired
    //     the lock on some earlier task, returned to the top of its loop,
    //     and went idle WITHOUT releasing it. The unlock was leaked.
    //   * The lock's (owner,state) and the holder's RIP stay byte-for-byte
    //     UNCHANGED across a multi-second window: the holder will never
    //     run again to release it. The lock is effectively leaked.
    //
    // This is NOT a lost wakeup, so SetEvent on any worker-ack does not
    // help (Layers 2-4 are powerless here -- the field logs confirm they
    // fire detect-only and the game still freezes). The only thing that
    // can unblock the victim is force-releasing the leaked lock itself.
    //
    // Mechanism: a watchdog thread (no inline hook -- there is no single
    // function to wrap) periodically scans suspended threads for one
    // contending a BSSpinLock at a resolved spin-retry site, follows that
    // lock's owner, and -- only when the owner is provably parked in a
    // kernel wait and the lock has been steady-state held for the full
    // dwell+recheck window -- force-releases the lock via an
    // InterlockedCompareExchange64 that is a no-op if anything changed in
    // the last instant. Force-releasing a PROVABLY-leaked lock is safe:
    // its "owner" never reaches its own release, so clearing it cannot
    // race a legitimate unlock; worst case it is a no-op if the victim was
    // really contending a different lock.
    //
    // Ships detect-only by default (logs the signature, alters nothing)
    // until the force-release path is field-validated.

    bool Install();
    void Stop();

}
