#include "PCH.h"
#include "snapshot/System.h"

#include <Psapi.h>

namespace FreezeLogger::Snapshot::System {

    namespace {
        std::string FormatBytes(std::uint64_t a_bytes) {
            constexpr std::uint64_t KB = 1024;
            constexpr std::uint64_t MB = KB * 1024;
            constexpr std::uint64_t GB = MB * 1024;
            if (a_bytes >= GB) return std::format("{:.2f} GiB", (double)a_bytes / GB);
            if (a_bytes >= MB) return std::format("{:.2f} MiB", (double)a_bytes / MB);
            if (a_bytes >= KB) return std::format("{:.2f} KiB", (double)a_bytes / KB);
            return std::format("{} B", a_bytes);
        }
    }

    void Write(std::ostream& a_os) {
        OSVERSIONINFOEXW osv{};
        osv.dwOSVersionInfoSize = sizeof(osv);
        // RtlGetVersion gives the real version even with manifest spoofing.
        using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
        if (auto ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
            if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                    ::GetProcAddress(ntdll, "RtlGetVersion"))) {
                fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osv));
            }
        }
        a_os << "OS:                Windows "
             << osv.dwMajorVersion << "." << osv.dwMinorVersion
             << " build " << osv.dwBuildNumber << "\n";

        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        if (::GlobalMemoryStatusEx(&mem)) {
            a_os << "Memory load:       " << mem.dwMemoryLoad << "%\n";
            a_os << "Total RAM:         " << FormatBytes(mem.ullTotalPhys) << "\n";
            a_os << "Available RAM:     " << FormatBytes(mem.ullAvailPhys) << "\n";
            a_os << "Total page file:   " << FormatBytes(mem.ullTotalPageFile) << "\n";
            a_os << "Avail page file:   " << FormatBytes(mem.ullAvailPageFile) << "\n";
        }

        PROCESS_MEMORY_COUNTERS_EX pm{};
        if (::GetProcessMemoryInfo(
                ::GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm),
                sizeof(pm)))
        {
            a_os << "Working set:       " << FormatBytes(pm.WorkingSetSize) << "\n";
            a_os << "Private bytes:     " << FormatBytes(pm.PrivateUsage) << "\n";
            a_os << "Page faults:       " << pm.PageFaultCount << "\n";
        }

        SYSTEM_INFO si{};
        ::GetNativeSystemInfo(&si);
        a_os << "Logical CPUs:      " << si.dwNumberOfProcessors << "\n";

        DWORD handleCount = 0;
        if (::GetProcessHandleCount(::GetCurrentProcess(), &handleCount)) {
            a_os << "Handle count:      " << handleCount << "\n";
        }
    }

}
