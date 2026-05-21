#include "PCH.h"
#include "Breaker.h"
#include "Config.h"
#include "Stats.h"

namespace WorkerSpinLockFix::Breaker {

    namespace {

        // Cycle signature: sorted set of (waiter_tid, lock_addr) pairs.
        // Two observations of the same wait topology produce the same
        // signature regardless of which spinning thread observed it.
        //
        // Stack-friendly fixed-size storage. Sized to the chain cap so we
        // can build, store, and compare signatures without ever calling
        // the heap. This is essential because OnCycleDetected runs from
        // inside the AcquireHook slow path; introducing a heap call there
        // creates a (heap CRITICAL_SECTION -> BSSpinLock) lock-order edge
        // that deadlocks against legitimate engine paths.
        struct Signature {
            using Pair = std::pair<DWORD, std::uintptr_t>;
            std::array<Pair, WaitGraph::kMaxHops> pairs{};
            int len{ 0 };

            bool operator==(const Signature& other) const noexcept {
                if (len != other.len) {
                    return false;
                }
                for (int i = 0; i < len; ++i) {
                    if (pairs[i] != other.pairs[i]) {
                        return false;
                    }
                }
                return true;
            }
        };

        Signature MakeSignature(
            const WaitGraph::CycleParticipant* cycle, int cycle_len) noexcept
        {
            Signature sig;
            const int n = std::min(cycle_len, WaitGraph::kMaxHops);
            for (int i = 0; i < n; ++i) {
                sig.pairs[i] = {
                    cycle[i].waiter,
                    reinterpret_cast<std::uintptr_t>(cycle[i].waiting_on)
                };
            }
            sig.len = n;
            std::sort(sig.pairs.begin(), sig.pairs.begin() + n);
            return sig;
        }

        struct RecentCycle {
            Signature      sig{};
            std::uint64_t  first_seen_ms{ 0 };
            std::uint64_t  last_seen_ms{ 0 };
            std::uint64_t  observations{ 0 };
            // True once any thread has taken responsibility for this
            // cycle's confirmation/break flow. Subsequent observers of
            // the same signature only bump observations and last_seen_ms.
            // This guarantees only one thread sleeps the confirmation
            // window per cycle, regardless of how many threads happened
            // to observe it concurrently.
            bool           breaker_claimed{ false };
            bool           confirmed_logged{ false };
            bool           in_use{ false };
        };

        constexpr std::size_t kRecentCapacity = 32;
        std::array<RecentCycle, kRecentCapacity> g_recent;
        std::mutex                                g_recent_mutex;

        // Eviction threshold: an entry that has not been observed for this
        // many ms is reclaimable. Confirmation_window x 4 + 100 ms gives
        // headroom for slow-running confirmation flows (sleep + verify +
        // CAS + log) so the responsible breaker's slot is never reaped
        // out from under it.
        std::uint64_t StaleAfterMs(std::uint32_t confirmation_window_ms) noexcept {
            const std::uint64_t base = std::max<std::uint32_t>(
                confirmation_window_ms, 1u);
            return base * 4 + 100;
        }

        // Returns the existing entry for sig, or nullptr if none. Caller
        // must hold g_recent_mutex.
        RecentCycle* FindLocked(const Signature& sig) noexcept {
            for (auto& entry : g_recent) {
                if (entry.in_use && entry.sig == sig) {
                    return &entry;
                }
            }
            return nullptr;
        }

        // Returns a slot to insert a new entry into. Prefers an unused
        // slot; otherwise evicts the slot with the oldest last_seen_ms.
        // Caller must hold g_recent_mutex.
        RecentCycle& AcquireSlotLocked() noexcept {
            RecentCycle* victim = &g_recent[0];
            for (auto& entry : g_recent) {
                if (!entry.in_use) {
                    return entry;
                }
                if (entry.last_seen_ms < victim->last_seen_ms) {
                    victim = &entry;
                }
            }
            return *victim;
        }

        void LogCycle(
            const char*                            event,
            const WaitGraph::CycleParticipant*     cycle,
            int                                    cycle_len,
            std::uint64_t                          age_ms,
            std::uint64_t                          observations)
        {
            logs::warn(
                "[CYCLE] {} (length={}, age={}ms, observations={}):",
                event, cycle_len, age_ms, observations);
            for (int i = 0; i < cycle_len; ++i) {
                const auto& p = cycle[i];
                logs::warn(
                    "[CYCLE]   TID {} waits on lock {} (owner TID {})",
                    p.waiter,
                    static_cast<void*>(p.waiting_on),
                    p.owner);
            }
        }

    } // namespace

    void Init() {
        std::lock_guard lk(g_recent_mutex);
        for (auto& entry : g_recent) {
            entry = RecentCycle{};
        }
    }

    // Confirmation flow (time-based):
    //
    // A naive observation-counting approach (confirm once a signature
    // has been seen N times within a window) would fail for a clean
    // 2-thread AB-BA: each thread enters BSSpinLock::Acquire exactly
    // once and then stays inside the engine's internal spin/SleepEx
    // loop forever, so only one observation arrives. The cycle would
    // be detected but never confirmed and never broken.
    //
    // The flow we use instead is:
    //
    //   1. First observer of a given signature claims it (breaker_claimed
    //      = true) and is responsible for the confirmation flow.
    //   2. The claiming thread sleeps for confirmation_window_ms outside
    //      the recent-cycle mutex (so concurrent observers can still
    //      record into the same slot).
    //   3. After the window, the claimer calls
    //      WaitGraph::VerifyCycleStillPresent. If the cycle resolved
    //      itself during the window, the claimer logs a "self-resolved"
    //      race and exits without breaking. Otherwise it CASes the
    //      victim lock's state from 1 -> 0 and unblocks the cycle.
    //   4. Subsequent observers of the same signature only bump the
    //      observations counter and last_seen_ms. They never sleep and
    //      never break. This caps the cost of any single cycle at one
    //      sleep + one verify + one CAS regardless of how many threads
    //      detect it.
    //
    // The slow path (this function) is called only from inside
    // BSSpinLock::Acquire when WouldFormCycle returns >= 2. We hold no
    // BSSpinLocks ourselves at that point (we are about to spin for one),
    // so taking g_recent_mutex (an SRWLock) here does not introduce a
    // BSSpinLock -> SRWLock lock-order edge. The sleep happens after
    // the mutex is released so it does not block other observers.
    void OnCycleDetected(
        DWORD                              me,
        WaitGraph::Lock*                /*target*/,
        const WaitGraph::CycleParticipant* cycle,
        int                                cycle_len)
    {
        if (cycle == nullptr || cycle_len < 2) {
            return;
        }

        const auto& cfg = Config::Get();
        const auto sig = MakeSignature(cycle, cycle_len);
        const auto now_ms = ::GetTickCount64();

        bool first_observation = false;
        bool we_are_breaker    = false;
        std::uint64_t age_ms        = 0;
        std::uint64_t observations  = 0;

        {
            std::lock_guard lk(g_recent_mutex);

            const auto stale_after = StaleAfterMs(cfg.confirmation_window_ms);
            for (auto& entry : g_recent) {
                if (entry.in_use &&
                    !entry.breaker_claimed &&
                    now_ms >= entry.last_seen_ms &&
                    now_ms - entry.last_seen_ms > stale_after)
                {
                    entry = RecentCycle{};
                }
            }

            RecentCycle* found = FindLocked(sig);
            if (found == nullptr) {
                auto& slot = AcquireSlotLocked();
                slot = RecentCycle{};
                slot.sig             = sig;
                slot.first_seen_ms   = now_ms;
                slot.last_seen_ms    = now_ms;
                slot.observations    = 1;
                slot.breaker_claimed = true;
                slot.confirmed_logged = false;
                slot.in_use          = true;
                first_observation    = true;
                we_are_breaker       = true;
                age_ms               = 0;
                observations         = 1;
            } else {
                found->last_seen_ms = now_ms;
                found->observations += 1;
                observations = found->observations;
                age_ms = (now_ms >= found->first_seen_ms)
                    ? (now_ms - found->first_seen_ms)
                    : 0;
                // Subsequent observers do NOT take the breaker role; the
                // first observer is responsible for the confirm + break
                // flow. We just bump counters here and return.
            }
        }

        if (first_observation) {
            Stats::OnCycleObserved();
            if (cfg.log_cycle_events) {
                LogCycle("first observation",
                    cycle, cycle_len, age_ms, observations);
            }
        }

        if (!we_are_breaker) {
            // Some other thread is already running the confirmation flow
            // for this signature. Nothing more to do here.
            return;
        }

        // We are the responsible breaker for this signature. Run the
        // confirmation flow OUTSIDE g_recent_mutex so concurrent
        // observers can still register their observations against this
        // slot.
        if (cfg.confirmation_window_ms > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(cfg.confirmation_window_ms));
        }

        // After the window, re-read the slot for stats / logging.
        std::uint64_t confirmed_age_ms = 0;
        std::uint64_t confirmed_observations = 0;
        {
            std::lock_guard lk(g_recent_mutex);
            RecentCycle* found = FindLocked(sig);
            if (found != nullptr) {
                const auto then = ::GetTickCount64();
                confirmed_age_ms = (then >= found->first_seen_ms)
                    ? (then - found->first_seen_ms)
                    : 0;
                confirmed_observations = found->observations;
                found->confirmed_logged = true;
            }
        }

        Stats::OnCycleConfirmed();
        if (cfg.log_cycle_events) {
            LogCycle(cfg.break_enabled
                         ? "confirmed (will break)"
                         : "confirmed (detect-only)",
                cycle, cycle_len, confirmed_age_ms, confirmed_observations);
        }

        if (!cfg.break_enabled) {
            Stats::OnBreakSuppressed();
            return;
        }

        // Re-verify the cycle is still topologically present. Engine
        // contention often resolves itself in microseconds; we do not
        // want to force-release a cycle that has already self-cleared
        // during the confirmation window.
        if (!WaitGraph::VerifyCycleStillPresent(cycle, cycle_len)) {
            Stats::OnBreakRaced();
            if (cfg.log_cycle_events) {
                logs::warn(
                    "[BREAK] cycle vanished before break (self-resolved). "
                    "Detector TID {}.", me);
            }
            return;
        }

        // Pick victim: chain[0].waiting_on. That's the lock the
        // detecting thread (`me`) was about to spin on. Its current
        // owner is the OTHER cycle participant. Force-releasing it
        // lets `me` proceed; the other thread continues to execute
        // its critical section without lock protection until it
        // releases (which is harmless if the engine's internal
        // invariants on this critical section don't depend on
        // mutual exclusion of those exact two operations).
        auto* victim_lock = cycle[0].waiting_on;
        const DWORD observed_owner = cycle[0].owner;
        if (victim_lock == nullptr) {
            Stats::OnBreakRaced();
            return;
        }

        auto* state_ptr = reinterpret_cast<volatile LONG*>(
            reinterpret_cast<std::uintptr_t>(victim_lock) +
            offsetof(WaitGraph::Lock, state));
        const LONG prev = ::InterlockedCompareExchange(
            state_ptr, 0, 1);
        if (prev == 1) {
            Stats::OnBreakDone();
            if (cfg.log_cycle_events) {
                logs::warn(
                    "[BREAK] force-released BSSpinLock 0x{:x} "
                    "(observed owner TID {}, state 1->0). "
                    "Detector TID {} should now acquire on next spin.",
                    reinterpret_cast<std::uintptr_t>(victim_lock),
                    observed_owner, me);
            }
        } else {
            Stats::OnBreakRaced();
            if (cfg.log_cycle_events) {
                logs::warn(
                    "[BREAK] CAS race on BSSpinLock 0x{:x} (state was "
                    "{} not 1). Lock self-released between confirm and "
                    "break. Detector TID {}.",
                    reinterpret_cast<std::uintptr_t>(victim_lock),
                    static_cast<int>(prev), me);
            }
        }
    }

}
