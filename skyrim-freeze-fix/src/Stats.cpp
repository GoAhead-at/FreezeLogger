#include "PCH.h"
#include "Stats.h"
#include "Config.h"

namespace WorkerSpinLockFix::Stats {

    namespace {

        struct LockCounters {
            std::atomic<std::uint64_t> reaped{ 0 };
            std::atomic<std::uint64_t> live_skips{ 0 };
            std::atomic<std::uint64_t> races{ 0 };
        };

        LockCounters g_lockA;
        LockCounters g_lockB;

        std::thread        g_dumpThread;
        std::atomic<bool>  g_running{ false };
        std::atomic<bool>  g_started{ false };

        LockCounters& CountersFor(std::string_view which) {
            if (which == "LockA") return g_lockA;
            return g_lockB;  // default to LockB for any other label
        }

        void DumpOnce() {
            const auto la_r  = g_lockA.reaped.load(std::memory_order_relaxed);
            const auto la_ls = g_lockA.live_skips.load(std::memory_order_relaxed);
            const auto la_rc = g_lockA.races.load(std::memory_order_relaxed);
            const auto lb_r  = g_lockB.reaped.load(std::memory_order_relaxed);
            const auto lb_ls = g_lockB.live_skips.load(std::memory_order_relaxed);
            const auto lb_rc = g_lockB.races.load(std::memory_order_relaxed);

            logs::info(
                "stats: LockA reaped={} live_skips={} races={}; "
                "LockB reaped={} live_skips={} races={}",
                la_r, la_ls, la_rc,
                lb_r, lb_ls, lb_rc);
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

    }

    void OnReaped(std::string_view which) {
        CountersFor(which).reaped.fetch_add(1, std::memory_order_relaxed);
    }

    void OnLiveSkip(std::string_view which) {
        CountersFor(which).live_skips.fetch_add(1, std::memory_order_relaxed);
    }

    void OnRace(std::string_view which) {
        CountersFor(which).races.fetch_add(1, std::memory_order_relaxed);
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
        g_dumpThread = std::thread(
            DumpLoop, std::chrono::seconds(s.stats_interval_s));
        g_dumpThread.detach();

        logs::info("Periodic stats dump started (interval = {}s).",
            s.stats_interval_s);
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
    }

}
