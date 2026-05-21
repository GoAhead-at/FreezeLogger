#include "PCH.h"
#include "Stats.h"
#include "Config.h"

namespace WorkerSpinLockFix::Stats {

    namespace {

        // AcquireHook (slow path only; see Stats.h)
        std::atomic<std::uint64_t> g_acq_slow{ 0 };

        // Breaker
        std::atomic<std::uint64_t> g_cycle_observed{ 0 };
        std::atomic<std::uint64_t> g_cycle_confirmed{ 0 };
        std::atomic<std::uint64_t> g_break_suppressed{ 0 };
        std::atomic<std::uint64_t> g_break_done{ 0 };
        std::atomic<std::uint64_t> g_break_raced{ 0 };

        // Reaper backstop
        std::atomic<std::uint64_t> g_reaper_scans{ 0 };
        std::atomic<std::uint64_t> g_stale_reaped{ 0 };
        std::atomic<std::uint64_t> g_force_race{ 0 };
        std::atomic<std::uint64_t> g_last_threads{ 0 };
        std::atomic<std::uint64_t> g_last_spinners{ 0 };
        std::atomic<std::uint64_t> g_last_candidates{ 0 };
        std::atomic<std::uint64_t> g_last_edges{ 0 };

        std::thread       g_dump_thread;
        std::atomic<bool> g_running{ false };
        std::atomic<bool> g_started{ false };

        void DumpOnce() {
            logs::info(
                "stats: acq_slow={} cycles_observed={} cycles_confirmed={} "
                "breaks_done={} breaks_raced={} breaks_suppressed={} | "
                "reaper: scans={} threads={} spinners={} candidates={} "
                "edges={} stale_reaped={} races={}",
                g_acq_slow.load(std::memory_order_relaxed),
                g_cycle_observed.load(std::memory_order_relaxed),
                g_cycle_confirmed.load(std::memory_order_relaxed),
                g_break_done.load(std::memory_order_relaxed),
                g_break_raced.load(std::memory_order_relaxed),
                g_break_suppressed.load(std::memory_order_relaxed),
                g_reaper_scans.load(std::memory_order_relaxed),
                g_last_threads.load(std::memory_order_relaxed),
                g_last_spinners.load(std::memory_order_relaxed),
                g_last_candidates.load(std::memory_order_relaxed),
                g_last_edges.load(std::memory_order_relaxed),
                g_stale_reaped.load(std::memory_order_relaxed),
                g_force_race.load(std::memory_order_relaxed));
        }

        void DumpLoop(std::chrono::seconds interval) {
            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(interval);
                if (!g_running.load(std::memory_order_relaxed)) {
                    break;
                }
                DumpOnce();
            }
        }

    } // namespace

    void OnAcquireSlow() noexcept {
        g_acq_slow.fetch_add(1, std::memory_order_relaxed);
    }

    void OnCycleObserved() noexcept {
        g_cycle_observed.fetch_add(1, std::memory_order_relaxed);
    }
    void OnCycleConfirmed() noexcept {
        g_cycle_confirmed.fetch_add(1, std::memory_order_relaxed);
    }
    void OnBreakSuppressed() noexcept {
        g_break_suppressed.fetch_add(1, std::memory_order_relaxed);
    }
    void OnBreakDone() noexcept {
        g_break_done.fetch_add(1, std::memory_order_relaxed);
    }
    void OnBreakRaced() noexcept {
        g_break_raced.fetch_add(1, std::memory_order_relaxed);
    }

    void OnReaperScan(
        std::size_t threads,
        std::size_t spinners,
        std::size_t candidates,
        std::size_t stable_edges) noexcept
    {
        g_reaper_scans.fetch_add(1, std::memory_order_relaxed);
        g_last_threads.store(static_cast<std::uint64_t>(threads), std::memory_order_relaxed);
        g_last_spinners.store(static_cast<std::uint64_t>(spinners), std::memory_order_relaxed);
        g_last_candidates.store(static_cast<std::uint64_t>(candidates), std::memory_order_relaxed);
        g_last_edges.store(static_cast<std::uint64_t>(stable_edges), std::memory_order_relaxed);
    }

    void OnStaleReaped() noexcept {
        g_stale_reaped.fetch_add(1, std::memory_order_relaxed);
    }
    void OnForceRace() noexcept {
        g_force_race.fetch_add(1, std::memory_order_relaxed);
    }

    void StartPeriodicDump() {
        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        const auto& s = Config::Get();
        if (s.stats_interval_s == 0) {
            logs::info("Periodic stats dump disabled (stats_interval_s=0).");
            return;
        }

        g_running.store(true, std::memory_order_relaxed);
        g_dump_thread = std::thread(
            DumpLoop, std::chrono::seconds(s.stats_interval_s));
        g_dump_thread.detach();

        logs::info("Periodic stats dump started (interval = {}s).",
            s.stats_interval_s);
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
    }

}
