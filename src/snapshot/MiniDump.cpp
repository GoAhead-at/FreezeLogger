#include "PCH.h"
#include "snapshot/MiniDump.h"

#include "Config.h"

#include <algorithm>
#include <chrono>

namespace FreezeLogger::Snapshot::MiniDump {

    namespace {

        void EnforceRetention(const std::filesystem::path& a_dir, std::uint32_t a_keep) {
            if (a_keep == 0) return;

            struct Entry {
                std::filesystem::path           path;
                std::filesystem::file_time_type mtime;
            };
            std::vector<Entry> entries;
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(a_dir, ec)) {
                if (!e.is_regular_file()) continue;
                const auto& fn = e.path().filename().string();
                if (fn.starts_with("freeze_") && fn.ends_with(".dmp")) {
                    entries.push_back({e.path(), e.last_write_time(ec)});
                }
            }
            if (entries.size() <= a_keep) return;

            std::sort(entries.begin(), entries.end(),
                      [](const Entry& l, const Entry& r){ return l.mtime > r.mtime; });

            for (std::size_t i = a_keep; i < entries.size(); ++i) {
                std::filesystem::remove(entries[i].path, ec);
            }
        }

        MINIDUMP_TYPE ParseFlags(std::string_view a_str) {
            std::uint32_t bits = MiniDumpNormal;
            std::string s{a_str};
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });

            if (s.find("threadinfo") != std::string::npos) {
                bits |= MiniDumpWithThreadInfo;
            }
            if (s.find("indirect") != std::string::npos) {
                bits |= MiniDumpWithIndirectlyReferencedMemory;
            }
            if (s.find("fullmemory") != std::string::npos) {
                bits |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
            }
            if (s.find("dataseg") != std::string::npos) {
                bits |= MiniDumpWithDataSegs;
            }
            return static_cast<MINIDUMP_TYPE>(bits);
        }

        std::string Timestamp() {
            using namespace std::chrono;
            const auto now    = system_clock::now();
            const auto time_t = system_clock::to_time_t(now);
            std::tm tm{};
            ::localtime_s(&tm, &time_t);
            return std::format("{:04}-{:02}-{:02}_{:02}{:02}{:02}",
                               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                               tm.tm_hour, tm.tm_min, tm.tm_sec);
        }

    }

    void Write(std::ostream& a_os, const std::filesystem::path& a_outputDir) {
        const auto& cfg = Config::Get().minidump;
        if (!cfg.enabled) {
            a_os << "<minidump disabled by config>\n";
            return;
        }

        const auto dumpDir = a_outputDir / "minidumps";
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);

        const auto path  = dumpDir / std::format("freeze_{}.dmp", Timestamp());
        const auto flags = ParseFlags(cfg.flags);

        HANDLE hFile = ::CreateFileW(
            path.wstring().c_str(),
            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE) {
            a_os << "<CreateFile failed: " << ::GetLastError() << ">\n";
            return;
        }

        const BOOL ok = ::MiniDumpWriteDump(
            ::GetCurrentProcess(),
            ::GetCurrentProcessId(),
            hFile,
            flags,
            nullptr, nullptr, nullptr);
        ::CloseHandle(hFile);

        if (!ok) {
            a_os << "<MiniDumpWriteDump failed: " << ::GetLastError() << ">\n";
            return;
        }
        a_os << "Mini-dump written: " << path.string() << "\n";
        a_os << "Flags:             0x" << std::format("{:x}", (std::uint32_t)flags) << "\n";

        EnforceRetention(dumpDir, cfg.retain_last_n);
    }

}
