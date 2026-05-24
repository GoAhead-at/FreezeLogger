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
            std::atomic<DWORD> tid{ 0 };
            std::atomic<Lock*> waiting_on{ nullptr };
        };

        std::array<ThreadSlot, kMaxThreads> g_slots;
        thread_local ThreadSlot*            tls_self = nullptr;

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

    void EnterSlow(DWORD tid, Lock* target) noexcept {
        auto* self = Self(tid);
        if (self == nullptr) {
            return;
        }
        self->waiting_on.store(target, std::memory_order_release);
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
            Lock* next_lock =
                owner_slot->waiting_on.load(std::memory_order_acquire);
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
            Lock* current_target =
                slot->waiting_on.load(std::memory_order_acquire);
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
        int n = 0;
        for (auto& slot : g_slots) {
            if (n >= cap) {
                break;
            }
            const DWORD tid =
                slot.tid.load(std::memory_order_acquire);
            Lock* const wait =
                slot.waiting_on.load(std::memory_order_acquire);
            if (tid != 0 && wait != nullptr) {
                out[n++] = EdgeView{ tid, wait };
            }
        }
        return n;
    }

}
