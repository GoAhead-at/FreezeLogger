#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace FreezeLogger::Config {

    struct Watchdog {
        // 15 s: middle ground.
        //   - Above 99 % of legitimate Nolvus save-loads / cell transitions.
        //   - Short enough that a real freeze is captured before most users
        //     give up and kill the process.
        //   - Earlier 30 s default missed real freezes when users force-killed
        //     within ~20-25 s; new default keeps that window comfortable.
        std::uint32_t threshold_ms        = 15000;
        std::uint32_t check_interval_ms   = 500;
        std::uint32_t snapshot_cooldown_s = 60;
        bool          annotate_on_resolve = true;
    };

    struct Snapshot {
        bool          include_threads        = true;
        bool          include_modules        = true;
        bool          include_papyrus        = true;
        bool          include_animgraph      = true;
        bool          include_engine         = true;
        bool          include_system         = true;
        bool          include_ringbuffer     = true;
        // Raised from 64 -> 1024: the freeze captured on 2026-05-17 had 381
        // total threads in the process, of which 256 were walked (the rest
        // dropped). Investigation of "dispatch+wait" deadlock signatures
        // requires seeing the *producer* thread that should have signaled
        // the main loop's wait; that thread can be anywhere in the pool.
        std::uint32_t max_threads            = 1024;
        // Raised from a hard-coded 256 -> 120 (configurable): per-thread
        // stack walks rarely exceed 30 frames in the captured snapshots,
        // but Skyrim's Bethesda netcode and Steam overlay threads can hit
        // ~80. 120 leaves headroom while shrinking total report size.
        std::uint32_t max_frames_per_stack   = 120;
    };

    struct RingBuffer {
        std::uint32_t papyrus_lines = 100;
        std::uint32_t skse_events   = 50;
    };

    struct MiniDump {
        // ON by default. The text report tells us a great deal, but a
        // .dmp lets the offline reader replay registers, walk arbitrary
        // memory, and disassemble code paths the live probes didn't
        // cover. Disk cost is bounded by retain_last_n; default normal+
        // threadinfo+indirect dumps are 30-90 MiB for a full Skyrim
        // process. Users on tight disks can flip this off in the TOML.
        bool          enabled       = true;
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

    struct TestMode {
        // OFF by default. When true, a lightweight background thread polls
        // for `hotkey_vk`; on each fresh press it writes a freeze report
        // on demand WITHOUT stalling the game. This is a developer/QA
        // convenience for validating report output (e.g. the Papyrus VM and
        // Animation-graph sections) on a live, healthy game — it is NOT a
        // synthetic-stall trigger and does not exercise the watchdog
        // detection path. Intended for internal/not-yet-public builds.
        bool          capture_on_pause = false;
        // Virtual-key code to listen for. Default 0x13 = VK_PAUSE.
        std::uint32_t hotkey_vk        = 0x13;
    };

    struct Root {
        Watchdog   watchdog;
        Snapshot   snapshot;
        RingBuffer ringbuffer;
        MiniDump   minidump;
        Output     output;
        Symbols    symbols;
        Logging    logging;
        TestMode   test_mode;
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
