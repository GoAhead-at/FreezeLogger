#pragma once

namespace WorkerSpinLockFix::Config {

    struct Settings {
        // Master kill-switch. If false, hooks are NOT installed and the
        // plugin reduces to a heartbeat in the log file. Useful for A/B
        // testing without rebuilding or removing the DLL.
        bool enabled{ true };

        // Log a warning whenever a hook waits this long or more on its
        // mutex before entering the wrapped section. 0 disables the
        // contention log. Default 1 ms is "any visible blocking".
        std::uint32_t contention_warn_ms{ 1 };

        // Periodic stats dump interval, in seconds. The plugin starts a
        // small worker thread that emits a one-line summary of the hook
        // counters. 0 disables the periodic dump.
        std::uint32_t stats_interval_s{ 60 };

        // If true, every hook entry/exit is logged at trace level. Spammy.
        // Off by default; flip on only for short controlled experiments.
        bool trace_each_call{ false };
    };

    void Init();
    const Settings& Get();

    // For tests / diagnostics.
    std::filesystem::path ConfigPath();

}
