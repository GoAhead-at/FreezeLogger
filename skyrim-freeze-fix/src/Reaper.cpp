#include "PCH.h"
#include "Reaper.h"
#include "AcquireHook.h"
#include "Config.h"
#include "Stats.h"
#include "WaitGraph.h"

// =============================================================================
// Stale-owner reaper (v2.0.3 redesign).
//
// Acts as a safety net under the AcquireHook + WaitGraph + Breaker pipeline.
// Picks up cases the entry-point hook can never see at runtime: threads
// that died holding a LockA / LockB while another thread is still waiting
// on it.
//
// Implementation: a periodic walk of the WaitGraph slot array. Every
// thread that has entered the AcquireHook slow path has registered a
// slot publishing its `(tid, waiting_on)` pair atomically. The reaper
// reads those slots, dereferences each `waiting_on` to inspect the
// BSSpinLock's `(owner, state)` pair, and -- after a stable-edge gate --
// force-releases any lock whose owner thread has died.
//
// The previous design (v2.0.1 and earlier) used SuspendThread +
// GetThreadContext on every process thread to scan stacks and registers
// for plausible BSSpinLock candidates. That design was retired because
// `GetThreadContext` is not bounded under adversarial kernel states and
// the only safe mitigations all carry hazards strictly worse than the
// hypothetical hang they would mitigate (abandonment leaves an engine
// thread suspended, which can CAUSE the freeze the plugin exists to
// prevent; TerminateThread on the helper corrupts the process). The
// full trade-off analysis is recorded in
// docs/case-study/26-reaper-snapshot-removed.md.
//
// Coverage trade-off: the reaper now only sees threads that traversed
// the AcquireHook slow path. Threads that took LockA / LockB via an
// inlined or non-`id 12210` acquire path are invisible to it. Phase 1.5
// (case-study doc 17) confirmed all six known acquirers go through
// `id 12210`, so the loss of coverage is theoretical against the bug
// this plugin targets.
// =============================================================================

namespace WorkerSpinLockFix::Reaper {

    namespace {

        struct ObservedEdge {
            DWORD          waiter{ 0 };
            std::uintptr_t lock_addr{ 0 };
            std::uint32_t  owner{ 0 };
            std::uint32_t  state{ 0 };
        };

        struct StableEdge {
            ObservedEdge   edge{};
            std::uint64_t  first_seen_ms{ 0 };
            std::uint64_t  last_probe_log_ms{ 0 };
            bool           seen_this_tick{ false };
        };

        std::atomic<bool> g_stop{ false };
        std::atomic<bool> g_started{ false };
        std::thread       g_thread;

        std::vector<StableEdge> g_edges;

        // An observed (waiter, lock, owner) edge must persist across
        // scans for at least kStaleStableMs before a stale-owner
        // force-release fires. Filters out transient races where the
        // waiter has already exited the slow path or the lock has
        // already been released by the time we re-check.
        constexpr std::uint64_t kStaleStableMs   = 2000;

        // Live-owner deadlock probe. After this many ms with the same
        // (waiter, lock, owner) edge holding while the owner is still
        // alive, log a diagnostic line so we can see deadlocks that
        // are NOT stale-owner -- e.g. owner is blocked on a non-
        // BSSpinLock primitive (a CRITICAL_SECTION, an event, an
        // inlined acquire path). These were previously enriched with
        // owner_rip / owner_module / owner_rcx fields obtained via
        // SnapshotThread; the v2.0.3 redesign drops those fields
        // because providing them would require re-introducing
        // SuspendThread + GetThreadContext on a live engine thread,
        // which is the exact hazard this redesign exists to
        // eliminate.
        constexpr std::uint64_t kLiveProbeMs     = 5000;
        constexpr std::uint64_t kLiveProbeRepeat = 30000;

        bool TryReadQword(std::uintptr_t addr, std::uintptr_t& out) noexcept {
            __try {
                out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out = 0;
                return false;
            }
        }

        bool TryReadSpinLock(
            std::uintptr_t addr,
            std::uint32_t& owner,
            std::uint32_t& state) noexcept
        {
            std::uintptr_t pair = 0;
            if (!TryReadQword(addr, pair)) {
                return false;
            }
            owner = static_cast<std::uint32_t>(pair & 0xffffffff);
            state = static_cast<std::uint32_t>(pair >> 32);
            return true;
        }

        bool IsThreadAlive(std::uint32_t tid) noexcept {
            if (tid == 0) {
                return false;
            }
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h) {
                return false;
            }
            DWORD code = 0;
            const BOOL ok = ::GetExitCodeThread(h, &code);
            ::CloseHandle(h);
            return ok && code == STILL_ACTIVE;
        }

        bool ForceRelease(std::uintptr_t lock_addr, std::uint32_t observed_state) {
            auto* state_ptr = reinterpret_cast<volatile LONG*>(lock_addr + 4);
            const LONG prev = ::InterlockedCompareExchange(
                state_ptr, 0, static_cast<LONG>(observed_state));
            return prev == static_cast<LONG>(observed_state);
        }

        bool SameEdge(const ObservedEdge& a, const ObservedEdge& b) noexcept {
            return a.waiter == b.waiter &&
                   a.lock_addr == b.lock_addr &&
                   a.owner == b.owner;
        }

        void UpdateStableEdges(
            const std::vector<ObservedEdge>& observed,
            std::uint64_t now_ms)
        {
            for (auto& existing : g_edges) {
                existing.seen_this_tick = false;
            }

            for (const auto& edge : observed) {
                auto it = std::find_if(g_edges.begin(), g_edges.end(),
                    [&edge](const StableEdge& s) {
                        return SameEdge(s.edge, edge);
                    });
                if (it == g_edges.end()) {
                    g_edges.push_back({ edge, now_ms, 0, true });
                } else {
                    it->edge = edge;
                    it->seen_this_tick = true;
                }
            }

            g_edges.erase(std::remove_if(g_edges.begin(), g_edges.end(),
                [](const StableEdge& s) { return !s.seen_this_tick; }),
                g_edges.end());
        }

        void ReapStaleObservedLocks(std::uint64_t now_ms) {
            for (auto& stable : g_edges) {
                if (stable.edge.owner == 0 || IsThreadAlive(stable.edge.owner)) {
                    continue;
                }
                if (now_ms < stable.first_seen_ms ||
                    now_ms - stable.first_seen_ms < kStaleStableMs)
                {
                    continue;
                }
                if (ForceRelease(stable.edge.lock_addr, stable.edge.state)) {
                    Stats::OnStaleReaped();
                    logs::warn(
                        "[REAPER] force-released observed stale BSSpinLock "
                        "0x{:x} (waiter TID {}, dead owner TID {}, state={}, "
                        "stable={}ms).",
                        stable.edge.lock_addr,
                        stable.edge.waiter,
                        stable.edge.owner,
                        stable.edge.state,
                        now_ms - stable.first_seen_ms);
                } else {
                    Stats::OnForceRace();
                }
            }
        }

        void LogLiveOwnerProbes(std::uint64_t now_ms) {
            for (auto& stable : g_edges) {
                if (stable.edge.owner == 0 || !IsThreadAlive(stable.edge.owner)) {
                    continue;
                }
                const auto held_ms = (now_ms < stable.first_seen_ms)
                    ? 0ull
                    : now_ms - stable.first_seen_ms;
                if (held_ms < kLiveProbeMs) {
                    continue;
                }
                if (stable.last_probe_log_ms != 0 &&
                    now_ms - stable.last_probe_log_ms < kLiveProbeRepeat)
                {
                    continue;
                }
                stable.last_probe_log_ms = now_ms;

                logs::warn(
                    "[REAPER] LIVE-OWNER WAIT held={}ms waiter=TID {} "
                    "lock=0x{:x} (state={}) owner=TID {} (alive). "
                    "Owner RIP / module / RCX intentionally omitted: the "
                    "v2.0.3 redesign retired SuspendThread / GetThreadContext "
                    "from the runtime path. See "
                    "docs/case-study/26-reaper-snapshot-removed.md.",
                    held_ms,
                    stable.edge.waiter,
                    stable.edge.lock_addr,
                    stable.edge.state,
                    stable.edge.owner);
            }
        }

        void Tick() {
            // Snapshot all currently-active wait edges. The 64-slot
            // bound matches WaitGraph::kMaxThreads; sizing the local
            // buffer to that bound keeps Tick allocation-free in the
            // steady state.
            constexpr int kMaxEdges = 64;
            WaitGraph::EdgeView edges[kMaxEdges];
            const int n = WaitGraph::SnapshotEdges(edges, kMaxEdges);

            std::vector<ObservedEdge> observed;
            observed.reserve(static_cast<std::size_t>(n));

            for (int i = 0; i < n; ++i) {
                const auto lock_addr =
                    reinterpret_cast<std::uintptr_t>(edges[i].waiting_on);

                std::uint32_t owner = 0;
                std::uint32_t state = 0;
                if (!TryReadSpinLock(lock_addr, owner, state)) {
                    continue;
                }
                // Filter:
                //   state == 0  -> lock no longer held; the waiter
                //                  will exit the slow path momentarily.
                //   owner == 0  -> ownership not yet recorded; race
                //                  with a release. Skip; we'll see it
                //                  next tick if it persists.
                //   owner == waiter -> recursive / self-edge; not a
                //                      stale-owner candidate.
                if (state == 0 || owner == 0 || owner == edges[i].waiter) {
                    continue;
                }
                observed.push_back({
                    edges[i].waiter,
                    lock_addr,
                    owner,
                    state
                });
            }

            const auto now_ms = ::GetTickCount64();
            UpdateStableEdges(observed, now_ms);

            // Stats fields preserved verbatim for compat with the
            // periodic dump line. With the v2.0.3 redesign the
            // semantics shift slightly:
            //   threads     -> active WaitGraph slots scanned this tick
            //   spinners    -> identical (every slot represents a
            //                  thread sitting in the AcquireHook slow
            //                  path)
            //   candidates  -> edges with a held foreign owner
            //                  (i.e. ObservedEdge instances)
            //   stable_edges -> the moving window's current size
            Stats::OnReaperScan(
                static_cast<std::size_t>(n),
                static_cast<std::size_t>(n),
                observed.size(),
                g_edges.size());

            ReapStaleObservedLocks(now_ms);
            LogLiveOwnerProbes(now_ms);
        }

        void ReaperBody(std::uint32_t interval_ms) {
            logs::info(
                "[REAPER] thread starting (interval={}ms, "
                "stale_stable={}ms; v2.0.3 WaitGraph-based scan, "
                "no SuspendThread / GetThreadContext on the runtime "
                "path).",
                interval_ms, kStaleStableMs);

            const auto interval = std::chrono::milliseconds(interval_ms);
            while (!g_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(interval);
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                Tick();
            }

            logs::info("[REAPER] thread exiting.");
        }

    } // namespace

    bool Install() {
        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        const auto& cfg = Config::Get();

        g_stop.store(false, std::memory_order_relaxed);
        try {
            g_thread = std::thread(ReaperBody, cfg.reaper_interval_ms);
            g_thread.detach();
        } catch (const std::exception& e) {
            logs::critical("[REAPER] failed to start: {}", e.what());
            g_started.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void Stop() {
        g_stop.store(true, std::memory_order_relaxed);
    }

}
