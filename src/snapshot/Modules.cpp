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
        };

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
                "  [{:016x}-{:016x}] {} {}\n",
                m.base, m.base + m.size,
                isSksePlugin ? "[skse]" : "      ",
                Narrow(m.path));
        }
    }

}
