#include "PCH.h"
#include "Stats.h"
#include "Config.h"

namespace WorkerSpinLockFix::Stats {

    namespace {

        struct EntryCounters {
            std::atomic<std::uint64_t> entries{ 0 };
            std::atomic<std::uint64_t> recursive_entries{ 0 };
            std::atomic<std::uint64_t> contended{ 0 };
            std::atomic<std::uint64_t> passthrough{ 0 };
        };

        struct ReaperCounters {
            std::atomic<std::uint64_t> reaped{ 0 };
            std::atomic<std::uint64_t> live_skips{ 0 };
            std::atomic<std::uint64_t> races{ 0 };
        };

        EntryCounters g_id19369;
        EntryCounters g_id40706;

        ReaperCounters g_lockA;
        ReaperCounters g_lockB;

        std::thread        g_dumpThread;
        std::atomic<bool>  g_running{ false };
        std::atomic<bool>  g_started{ false };

        EntryCounters& EntryCountersFor(std::string_view which) {
            if (which == "id_19369") return g_id19369;
            return g_id40706;
        }

        ReaperCounters& ReaperCountersFor(std::string_view which) {
            if (which == "LockA") return g_lockA;
            return g_lockB;
        }

        void DumpOnce() {
            const auto e_a  = g_id19369.entries.load(std::memory_order_relaxed);
            const auto er_a = g_id19369.recursive_entries.load(std::memory_order_relaxed);
            const auto c_a  = g_id19369.contended.load(std::memory_order_relaxed);
            const auto pt_a = g_id19369.passthrough.load(std::memory_order_relaxed);
            const auto e_b  = g_id40706.entries.load(std::memory_order_relaxed);
            const auto er_b = g_id40706.recursive_entries.load(std::memory_order_relaxed);
            const auto c_b  = g_id40706.contended.load(std::memory_order_relaxed);
            const auto pt_b = g_id40706.passthrough.load(std::memory_order_relaxed);

            const auto la_r  = g_lockA.reaped.load(std::memory_order_relaxed);
            const auto la_ls = g_lockA.live_skips.load(std::memory_order_relaxed);
            const auto la_rc = g_lockA.races.load(std::memory_order_relaxed);
            const auto lb_r  = g_lockB.reaped.load(std::memory_order_relaxed);
            const auto lb_ls = g_lockB.live_skips.load(std::memory_order_relaxed);
            const auto lb_rc = g_lockB.races.load(std::memory_order_relaxed);

            logs::info(
                "stats: id_19369 entries={} recursive={} contended={} passthrough={}; "
                "id_40706 entries={} recursive={} contended={} passthrough={}; "
                "reaper: LockA reaped={} live_skips={} races={}; "
                "LockB reaped={} live_skips={} races={}",
                e_a, er_a, c_a, pt_a,
                e_b, er_b, c_b, pt_b,
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

    void OnEntry(std::string_view which) {
        EntryCountersFor(which).entries.fetch_add(1, std::memory_order_relaxed);
    }

    void OnRecursiveEntry(std::string_view which) {
        EntryCountersFor(which).recursive_entries.fetch_add(1, std::memory_order_relaxed);
    }

    void OnContended(std::string_view which) {
        EntryCountersFor(which).contended.fetch_add(1, std::memory_order_relaxed);
    }

    void OnPassthrough(std::string_view which) {
        EntryCountersFor(which).passthrough.fetch_add(1, std::memory_order_relaxed);
    }

    void OnReaped(std::string_view which) {
        ReaperCountersFor(which).reaped.fetch_add(1, std::memory_order_relaxed);
    }

    void OnLiveSkip(std::string_view which) {
        ReaperCountersFor(which).live_skips.fetch_add(1, std::memory_order_relaxed);
    }

    void OnRace(std::string_view which) {
        ReaperCountersFor(which).races.fetch_add(1, std::memory_order_relaxed);
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
