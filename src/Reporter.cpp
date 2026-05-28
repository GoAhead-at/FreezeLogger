#include "PCH.h"
#include "Reporter.h"

#include "AddrLib.h"
#include "Config.h"
#include "Symbols.h"
#include "snapshot/AnimGraph.h"
#include "snapshot/Engine.h"
#include "snapshot/MiniDump.h"
#include "snapshot/Modules.h"
#include "snapshot/Papyrus.h"
#include "snapshot/System.h"
#include "snapshot/TaskPool.h"
#include "snapshot/Threads.h"
#include "snapshot/Verdict.h"
#include "snapshot/WaitGraph.h"
#include "RingBuffer.h"

#include <fstream>

namespace FreezeLogger::Reporter {

    namespace {

        std::filesystem::path g_lastReportPath;

        std::string TimestampForFilename() {
            using namespace std::chrono;
            const auto now    = system_clock::now();
            const auto time_t = system_clock::to_time_t(now);
            std::tm tm{};
            ::localtime_s(&tm, &time_t);
            return std::format("{:04}-{:02}-{:02}_{:02}{:02}{:02}",
                               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                               tm.tm_hour, tm.tm_min, tm.tm_sec);
        }

        std::string TimestampHuman() {
            using namespace std::chrono;
            const auto now    = system_clock::now();
            const auto time_t = system_clock::to_time_t(now);
            std::tm tm{};
            ::localtime_s(&tm, &time_t);
            return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} (local)",
                               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                               tm.tm_hour, tm.tm_min, tm.tm_sec);
        }

        void Flush(std::ofstream& a_os) {
            a_os.flush();
        }

        void WriteHeader(
            std::ofstream&          a_os,
            Watchdog::StalledThread a_stalled,
            std::uint64_t           a_mainAgeMs,
            std::uint64_t           a_renderAgeMs)
        {
            const auto& cfg = Config::Get();
            a_os << "================================================================\n";
            a_os << "FreezeLogger v" << FL_VERSION_MAJOR << "." << FL_VERSION_MINOR << "." << FL_VERSION_PATCH << "\n";
            a_os << "Captured:        " << TimestampHuman() << "\n";
            a_os << "Runtime:         Skyrim SE 1.5.97\n";
            a_os << "Stalled thread:  " << Watchdog::ToString(a_stalled) << "\n";
            a_os << "Main age:        " << a_mainAgeMs   << " ms\n";
            a_os << "Render age:      " << a_renderAgeMs << " ms\n";
            a_os << "Threshold:       " << cfg.watchdog.threshold_ms << " ms\n";
            a_os << "Symbol server:   " << (Symbols::SearchPath().empty() ? "(none)" : Symbols::SearchPath()) << "\n";
            a_os << "Address Library: "
                 << (AddrLib::Available() ? "loaded - " : "<unavailable> - ")
                 << AddrLib::DiagnosticString() << "\n";
            a_os << "================================================================\n\n";
        }

        // Inner body that handles C++ exceptions. MUST NOT contain any SEH —
        // MSVC forbids __try/__except in functions with C++ unwinding.
        template <typename Fn>
        void SectionInner(std::ofstream& a_os, Fn& a_fn) {
            try {
                a_fn(a_os);
            } catch (const std::exception& e) {
                a_os << "<unavailable: caught C++ exception: " << e.what() << ">\n";
            } catch (...) {
                a_os << "<unavailable: caught unknown C++ exception>\n";
            }
        }

        // SEH-only wrapper. Body captures the exception code into an out-
        // parameter and does NOTHING else — that's required because every
        // operation in a function containing __try/__except must avoid
        // creating temporaries with destructors (MSVC's "object unwinding
        // needed" rule). Formatting the SEH message is done by the caller
        // outside any __try block.
        template <typename Fn>
        void SectionWithSeh(std::ofstream& a_os, Fn& a_fn, DWORD& a_outCode) noexcept {
            a_outCode = 0;
            __try {
                SectionInner(a_os, a_fn);
            } __except (a_outCode = ::GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        template <typename Fn>
        void Section(std::ofstream& a_os, std::string_view a_title, Fn a_fn) {
            a_os << "## " << a_title << "\n";
            DWORD sehCode = 0;
            SectionWithSeh(a_os, a_fn, sehCode);
            if (sehCode != 0) {
                a_os << "<unavailable: caught SEH 0x"
                     << std::format("{:08x}", sehCode) << ">\n";
            }
            a_os << "\n";
            Flush(a_os);
        }

        std::filesystem::path EnsureOutputDir() {
            const auto dir = Config::ResolvedOutputDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            return dir;
        }

    }

    void CaptureAndWrite(
        Watchdog::StalledThread a_stalledThread,
        std::uint64_t           a_mainAgeMs,
        std::uint64_t           a_renderAgeMs)
    {
        const auto outputDir = EnsureOutputDir();
        if (outputDir.empty()) {
            logs::error("Reporter: no output directory could be resolved.");
            return;
        }

        const auto filename = std::format("freeze_{}_{}.log",
                                          TimestampForFilename(),
                                          Watchdog::ToString(a_stalledThread));
        const auto reportPath = outputDir / filename;

        std::ofstream os(reportPath, std::ios::out | std::ios::trunc);
        if (!os) {
            logs::error("Reporter: failed to open '{}' for write.", reportPath.string());
            return;
        }

        WriteHeader(os, a_stalledThread, a_mainAgeMs, a_renderAgeMs);

        const auto& cfg = Config::Get();

        // Verdict runs first so the human reader sees the freeze
        // classification before scrolling through hundreds of KB of
        // thread dumps. It is intentionally the cheap subset of the
        // diagnostics that the long-form sections below (MainWaitProbe,
        // WaitGraph) compute in full.
        Section(os, "Freeze classification", [](auto& s){ Snapshot::Verdict::Write(s); });

        if (cfg.snapshot.include_system) {
            Section(os, "System",         [](auto& s){ Snapshot::System::Write(s);    });
        }
        if (cfg.snapshot.include_threads) {
            Section(os, "Threads",        [](auto& s){ Snapshot::Threads::Write(s);   });
        }
        if (cfg.snapshot.include_modules) {
            Section(os, "Loaded modules", [](auto& s){ Snapshot::Modules::Write(s);   });
        }
        if (cfg.snapshot.include_papyrus) {
            Section(os, "Papyrus VM",     [](auto& s){ Snapshot::Papyrus::Write(s);   });
        }
        if (cfg.snapshot.include_animgraph) {
            Section(os, "Animation graph (player, lite)",
                          [](auto& s){ Snapshot::AnimGraph::Write(s); });
        }
        if (cfg.snapshot.include_engine) {
            Section(os, "Engine state",   [](auto& s){ Snapshot::Engine::Write(s);    });
        }
        // Task-pool snapshot runs after Engine state (which already
        // contains the long-form MainWaitProbe) and before the Wait
        // graph cross-tabulation. The intent — see docs/spec.md §6.9.5 —
        // is to expose which layer of Singleton-B was torn down between
        // the last healthy frame and the freeze instant. Cheap to run;
        // the heavy lifting (periodic capture) happens on the main
        // thread during normal frames.
        Section(os, "Task pool snapshot",
                [](auto& s){ Snapshot::TaskPool::Write(s); });
        if (cfg.snapshot.include_threads) {
            // The wait graph is a thread-cross-cut and only meaningful when
            // we already have permission to OpenThread/SuspendThread; gate
            // it on the same toggle as the per-thread walks.
            Section(os, "Wait graph",     [](auto& s){ Snapshot::WaitGraph::Write(s); });
        }
        if (cfg.snapshot.include_ringbuffer) {
            Section(os, "Recent activity",[](auto& s){ RingBuffer::WriteSnapshot(s);  });
        }
        if (cfg.minidump.enabled) {
            Section(os, "Mini-dump",
                    [&outputDir](auto& s){ Snapshot::MiniDump::Write(s, outputDir); });
        }

        os.close();
        g_lastReportPath = reportPath;

        // Also rewrite freeze_latest.log as a copy of this report.
        std::error_code ec;
        std::filesystem::copy_file(
            reportPath, outputDir / "freeze_latest.log",
            std::filesystem::copy_options::overwrite_existing, ec);

        EnforceRetention();

        logs::info("Wrote freeze report '{}'.", reportPath.string());
    }

    void AnnotateLatestResolved(std::uint64_t a_resolvedAfterMs) {
        if (g_lastReportPath.empty() || !std::filesystem::exists(g_lastReportPath)) {
            logs::warn("AnnotateLatestResolved: no prior report to annotate.");
            return;
        }
        std::ofstream os(g_lastReportPath, std::ios::out | std::ios::app);
        if (!os) return;
        os << "\n## Resolution\nResolved at T+" << a_resolvedAfterMs << " ms\n";
        os.close();
    }

    void EnforceRetention() {
        const auto& cfg = Config::Get().output;
        const auto dir = Config::ResolvedOutputDir();
        if (dir.empty() || cfg.keep_last_n_reports == 0) return;

        struct Entry {
            std::filesystem::path path;
            std::filesystem::file_time_type mtime;
        };
        std::vector<Entry> entries;
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (!e.is_regular_file()) continue;
            const auto& fn = e.path().filename().string();
            if (fn.starts_with("freeze_") && fn.ends_with(".log") &&
                fn != "freeze_latest.log") {
                entries.push_back({e.path(), e.last_write_time(ec)});
            }
        }
        if (entries.size() <= cfg.keep_last_n_reports) return;

        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b){ return a.mtime > b.mtime; });

        for (std::size_t i = cfg.keep_last_n_reports; i < entries.size(); ++i) {
            std::filesystem::remove(entries[i].path, ec);
        }
    }

}
