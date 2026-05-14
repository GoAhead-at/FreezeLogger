#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace FreezeLogger::Config {

    struct Watchdog {
        // 30 s: high enough to ride out heavy save-loads / cell transitions
        // on big modlists (Nolvus etc.) without false-positive snapshots.
        std::uint32_t threshold_ms        = 30000;
        std::uint32_t check_interval_ms   = 500;
        std::uint32_t snapshot_cooldown_s = 60;
        bool          annotate_on_resolve = true;
    };

    struct Snapshot {
        bool          include_threads     = true;
        bool          include_modules     = true;
        bool          include_papyrus     = true;
        bool          include_animgraph   = true;
        bool          include_engine      = true;
        bool          include_system      = true;
        bool          include_ringbuffer  = true;
        std::uint32_t max_threads         = 64;
    };

    struct RingBuffer {
        std::uint32_t papyrus_lines = 100;
        std::uint32_t skse_events   = 50;
    };

    struct MiniDump {
        bool          enabled       = false;
        std::string   flags         = "normal+threadinfo+indirect";
        std::uint32_t retain_last_n = 5;
    };

    struct Output {
        std::filesystem::path directory;          // empty = default location
        std::uint32_t         keep_last_n_reports = 50;
    };

    struct Symbols {
        // OFF by default: the first symbol resolution after a Windows update
        // can block for many seconds while DbgHelp downloads PDBs from
        // Microsoft's server; that's unacceptable on the watchdog snapshot
        // path because the threads we're walking are suspended. Users
        // chasing a specific freeze can flip this on in the TOML.
        bool                  use_ms_symbol_server = false;
        std::filesystem::path cache_directory;     // empty = <output>/symbols
    };

    struct Logging {
        std::string level = "info";
    };

    struct Root {
        Watchdog   watchdog;
        Snapshot   snapshot;
        RingBuffer ringbuffer;
        MiniDump   minidump;
        Output     output;
        Symbols    symbols;
        Logging    logging;
    };

    void        Load();           // reads Data/SKSE/Plugins/FreezeLogger.toml; falls back to defaults
    const Root& Get() noexcept;   // returns the loaded (or default) config

    // Resolved output path. Either Output::directory or
    // Documents/My Games/Skyrim Special Edition/SKSE/FreezeLogger/.
    std::filesystem::path ResolvedOutputDir();

    // Resolved symbol-cache path. Either Symbols::cache_directory or
    // <ResolvedOutputDir>/symbols.
    std::filesystem::path ResolvedSymbolCacheDir();

}
