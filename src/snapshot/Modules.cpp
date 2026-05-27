#include "PCH.h"
#include "snapshot/Modules.h"

#include <Psapi.h>

#include <algorithm>
#include <vector>

namespace FreezeLogger::Snapshot::Modules {

    namespace {

        struct Mod {
            HMODULE         handle;
            std::uintptr_t  base;
            std::uintptr_t  size;
            std::wstring    path;
            std::string     fileVersion;   // "A.B.C.D" from VS_FIXEDFILEINFO,
                                            // empty when unreadable
        };

        // Best-effort read of the file's VS_FIXEDFILEINFO. Empty string on
        // failure (binary lacks a VERSIONINFO resource, or the resource is
        // malformed). Linked via Version.lib in CMakeLists.txt.
        std::string ReadFileVersion(const wchar_t* a_path) {
            DWORD handle = 0;
            const DWORD size = ::GetFileVersionInfoSizeW(a_path, &handle);
            if (size == 0) return {};
            std::vector<std::byte> buffer(size);
            if (!::GetFileVersionInfoW(a_path, 0, size, buffer.data())) return {};
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if (!::VerQueryValueW(buffer.data(), L"\\",
                                  reinterpret_cast<LPVOID*>(&info), &len) ||
                !info || len < sizeof(VS_FIXEDFILEINFO))
            {
                return {};
            }
            const auto hi = info->dwFileVersionMS;
            const auto lo = info->dwFileVersionLS;
            return std::format("{}.{}.{}.{}",
                               (hi >> 16) & 0xffff,
                               hi & 0xffff,
                               (lo >> 16) & 0xffff,
                               lo & 0xffff);
        }

        std::wstring LowerInvariant(std::wstring s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](wchar_t c){ return std::towlower(c); });
            return s;
        }

        std::string Narrow(const std::wstring& w) {
            if (w.empty()) return {};
            const int n = ::WideCharToMultiByte(
                CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            ::WideCharToMultiByte(
                CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
            return out;
        }

    }

    void Write(std::ostream& a_os) {
        DWORD needed = 0;
        ::EnumProcessModulesEx(::GetCurrentProcess(), nullptr, 0, &needed,
                               LIST_MODULES_ALL);
        if (needed == 0) {
            a_os << "<EnumProcessModulesEx returned 0 bytes>\n";
            return;
        }

        std::vector<HMODULE> handles(needed / sizeof(HMODULE));
        if (!::EnumProcessModulesEx(
                ::GetCurrentProcess(),
                handles.data(),
                (DWORD)(handles.size() * sizeof(HMODULE)),
                &needed, LIST_MODULES_ALL))
        {
            a_os << "<EnumProcessModulesEx failed: "
                 << ::GetLastError() << ">\n";
            return;
        }
        handles.resize(needed / sizeof(HMODULE));

        std::vector<Mod> mods;
        mods.reserve(handles.size());
        for (HMODULE h : handles) {
            wchar_t path[MAX_PATH] = {};
            if (!::GetModuleFileNameExW(::GetCurrentProcess(), h, path, MAX_PATH)) {
                continue;
            }
            MODULEINFO mi{};
            if (!::GetModuleInformation(::GetCurrentProcess(), h, &mi, sizeof(mi))) {
                continue;
            }
            mods.push_back({
                h,
                reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll),
                static_cast<std::uintptr_t>(mi.SizeOfImage),
                path,
                ReadFileVersion(path),
            });
        }

        std::sort(mods.begin(), mods.end(),
                  [](const Mod& a, const Mod& b){ return a.base < b.base; });

        a_os << std::format("{} loaded modules:\n", mods.size());
        for (const auto& m : mods) {
            const auto pathLower = LowerInvariant(m.path);
            const bool isSksePlugin =
                pathLower.find(L"\\skse\\plugins\\") != std::wstring::npos;

            a_os << std::format(
                "  [{:016x}-{:016x}] {} {}",
                m.base, m.base + m.size,
                isSksePlugin ? "[skse]" : "      ",
                Narrow(m.path));
            if (!m.fileVersion.empty()) {
                a_os << "  (FileVersion " << m.fileVersion << ")";
            }
            a_os << "\n";
        }
    }

}
