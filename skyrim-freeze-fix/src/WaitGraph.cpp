#include "PCH.h"
#include "WaitGraph.h"

namespace WorkerSpinLockFix::WaitGraph {

    namespace {

        // Hard cap on how many threads we track concurrently. Skyrim normally
        // runs ~16 worker threads + main + a few render/audio threads, so 64
        // slots is comfortable. If the process ever exceeds this we silently
        // fall back to per-thread "no graph entry" without observing those
        // threads' wait state. The wait-graph is then an under-approximation;
        // detection may miss cycles that include those threads but the
        // stale-owner reaper still acts as a backstop.
        constexpr std::size_t kMaxThreads = 64;

        struct alignas(64) ThreadSlot {
            std::atomic<DWORD>         tid{ 0 };
            std::atomic<Lock*>         waiting_on{ nullptr };
            std::atomic<std::uint64_t> last_seen_ms{ 0 };
        };

        std::array<ThreadSlot, kMaxThreads> g_slots;
        thread_local ThreadSlot*            tls_self = nullptr;

        // A published wait edge is only honoured for this long after its
        // last refresh. The spin-retry hook (BSSpinLock::Acquire +0x8a)
        // refreshes a genuinely-stuck thread's edge on every outer backoff
        // iteration -- in the steady deadlock state that is once per the
        // engine's Sleep(1)-class yield, i.e. roughly every 1-16 ms
        // depending on the process timer resolution. 50 ms gives at least a
        // ~3x margin over the slowest refresh cadence, so a thread that is
        // really still spinning never looks stale, while a thread that
        // acquired its lock and moved on (and therefore stops hitting the
        // retry site) drops out of the graph within 50 ms.
        //
        // This window is the replacement for the exact ExitSlow bracket the
        // old entry-point hook provided. CRITICAL invariant: the Breaker's
        // confirmation_window_ms is configured LONGER than this value (see
        // Config.h / WorkerSpinLockFix.toml), so any edge that stops
        // refreshing during the confirmation sleep has provably expired by
        // the time VerifyCycleStillPresent runs. That is what prevents a
        // moved-on thread's lingering edge from causing a spurious break.
        constexpr std::uint64_t kEdgeStaleMs = 50;

        std::uint64_t NowMs() noexcept {
            return ::GetTickCount64();
        }

        // Load `slot`'s published wait target, but only if it was refreshed
        // within kEdgeStaleMs of `now`. Returns nullptr for an empty or
        // expired slot. Clock-skew safe: if `seen` is somehow ahead of
        // `now` the edge is treated as fresh rather than expired.
        Lock* LiveWaitingOn(const ThreadSlot& slot, std::uint64_t now) noexcept {
            Lock* w = slot.waiting_on.load(std::memory_order_acquire);
            if (w == nullptr) {
                return nullptr;
            }
            const std::uint64_t seen =
                slot.last_seen_ms.load(std::memory_order_acquire);
            if (now > seen && now - seen > kEdgeStaleMs) {
                return nullptr;
            }
            return w;
        }

        // Lazily registers a slot for the current thread. May fail if all
        // slots are taken; in that case it returns nullptr and the thread
        // is invisible to the wait graph for the rest of its life.
        ThreadSlot* RegisterSelf(DWORD me) noexcept {
            for (auto& slot : g_slots) {
                DWORD existing = slot.tid.load(std::memory_order_acquire);
                if (existing == me) {
                    return &slot;
                }
                if (existing == 0) {
                    DWORD expected = 0;
                    if (slot.tid.compare_exchange_strong(
                            expected, me,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire))
                    {
                        return &slot;
                    }
                    if (expected == me) {
                        return &slot;
                    }
                }
            }
            return nullptr;
        }

        ThreadSlot* LookupTid(DWORD tid) noexcept {
            for (auto& slot : g_slots) {
                if (slot.tid.load(std::memory_order_acquire) == tid) {
                    return &slot;
                }
            }
            return nullptr;
        }

        ThreadSlot* Self(DWORD me) noexcept {
            if (tls_self != nullptr) {
                return tls_self;
            }
            tls_self = RegisterSelf(me);
            return tls_self;
        }

    } // namespace

    void Init() {
        // Touching the array is enough; std::atomic<>'s default ctor leaves
        // values at zero/null which is what we want.
    }

    bool EnterSlow(DWORD tid, Lock* target) noexcept {
        auto* self = Self(tid);
        if (self == nullptr) {
            return false;
        }
        Lock* const prev = self->waiting_on.load(std::memory_order_acquire);
        // Stamp the liveness time BEFORE publishing the edge. The release
        // store on `waiting_on` then publishes the fresh timestamp to any
        // reader that acquire-loads `waiting_on` and sees this target.
        self->last_seen_ms.store(NowMs(), std::memory_order_release);
        self->waiting_on.store(target, std::memory_order_release);
        return prev != target;
    }

    void ExitSlow(DWORD tid) noexcept {
        auto* self = Self(tid);
        if (self == nullptr) {
            return;
        }
        self->waiting_on.store(nullptr, std::memory_order_release);
    }

    int WouldFormCycle(
        DWORD               me,
        Lock*               target,
        CycleParticipant*   out,
        int                 max_hops) noexcept
    {
        if (target == nullptr || out == nullptr || max_hops <= 0) {
            return 0;
        }

        // `target` is the lock `me` is spinning on right now (read live from
        // the hook), so it needs no staleness gate. Every subsequent hop
        // follows another thread's PUBLISHED edge, which must be live
        // (refreshed within kEdgeStaleMs) to be trusted -- otherwise a
        // thread that already acquired its lock and moved on could
        // contribute a phantom link to the chain.
        const std::uint64_t now = NowMs();

        DWORD waiter = me;
        Lock* lock   = target;
        int   len    = 0;

        for (int hop = 0; hop < max_hops; ++hop) {
            const DWORD owner = lock->owner;
            if (owner == 0) {
                return 0;
            }

            out[len++] = { waiter, lock, owner };

            if (owner == me) {
                return (len >= 2) ? len : 0;
            }

            if (len >= max_hops) {
                return 0;
            }

            ThreadSlot* owner_slot = LookupTid(owner);
            if (owner_slot == nullptr) {
                return 0;
            }
            Lock* next_lock = LiveWaitingOn(*owner_slot, now);
            if (next_lock == nullptr) {
                return 0;
            }

            waiter = owner;
            lock   = next_lock;
        }

        return 0;
    }

    bool VerifyCycleStillPresent(
        const CycleParticipant* cycle, int cycle_len) noexcept
    {
        if (cycle == nullptr || cycle_len < 2) {
            return false;
        }
        // This runs AFTER the breaker has slept confirmation_window_ms,
        // which is configured longer than kEdgeStaleMs. Each participant's
        // edge is therefore required to still be LIVE: a thread that stopped
        // spinning during the confirmation window (because it acquired the
        // lock, or because the cycle was a phantom built from a stale edge)
        // has, by construction, let its edge expire and fails this check.
        const std::uint64_t now = NowMs();
        for (int i = 0; i < cycle_len; ++i) {
            const auto& p = cycle[i];
            if (p.waiting_on == nullptr) {
                return false;
            }
            const DWORD current_owner = p.waiting_on->owner;
            if (current_owner != p.owner) {
                return false;
            }
            ThreadSlot* slot = LookupTid(p.waiter);
            if (slot == nullptr) {
                return false;
            }
            Lock* current_target = LiveWaitingOn(*slot, now);
            if (current_target != p.waiting_on) {
                return false;
            }
        }
        return true;
    }

    int SnapshotEdges(EdgeView* out, int cap) noexcept {
        if (out == nullptr || cap <= 0) {
            return 0;
        }
        const std::uint64_t now = NowMs();
        int n = 0;
        for (auto& slot : g_slots) {
            if (n >= cap) {
                break;
            }
            const DWORD tid =
                slot.tid.load(std::memory_order_acquire);
            if (tid == 0) {
                continue;
            }
            Lock* const wait = LiveWaitingOn(slot, now);
            if (wait != nullptr) {
                out[n++] = EdgeView{ tid, wait };
            }
        }
        return n;
    }

}
