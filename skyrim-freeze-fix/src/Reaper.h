#pragma once

namespace WorkerSpinLockFix::Reaper {

    // Stale-owner reaper. Periodically:
    //   - enumerates all process threads,
    //   - identifies threads spinning in BSSpinLock::Acquire by stack
    //     pattern (the spin-retry RVA from AcquireHook),
    //   - collects plausible BSSpinLock candidates from registers and
    //     stack windows,
    //   - if an observed lock is held by a TID that is no longer alive,
    //     force-releases it via CAS(state -> 0).
    //
    // Acts as a safety net under the entry-point hook for cases the hook
    // cannot see (threads that died holding a lock, indirect dispatches).
    // The poll interval is configurable; see Config::reaper_interval_ms.
    bool Install();

    void Stop();

}
