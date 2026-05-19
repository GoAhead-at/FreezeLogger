#include "PCH.h"
#include "AddrLib.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <vector>

namespace FreezeLogger::AddrLib {

    namespace {

        struct Entry {
            std::uintptr_t rva;
            std::uint32_t  id;
        };

        std::vector<Entry>   g_entries;        // sorted by rva
        std::uintptr_t       g_skyrimBase = 0;
        std::atomic<bool>    g_available{false};
        std::string          g_diagnostic{"<not initialised>"};

        std::filesystem::path SkyrimDataDir() {
            wchar_t buf[MAX_PATH] = {};
            const auto h = ::GetModuleHandleW(L"SkyrimSE.exe");
            if (!h) return {};
            if (::GetModuleFileNameW(h, buf, MAX_PATH) == 0) return {};
            std::filesystem::path exe(buf);
            return exe.parent_path() / L"Data";
        }

        std::filesystem::path FindAddrLibBin() {
            const auto data = SkyrimDataDir();
            if (data.empty()) return {};
            const auto p = data / L"SKSE" / L"Plugins" / L"version-1-5-97-0.bin";
            std::error_code ec;
            if (std::filesystem::exists(p, ec)) return p;
            return {};
        }

        // Decoder mirrors REL::IDDatabase::unpack_file (and analysis/
        // addrlib_lookup.py). Format v1 is what 1.5.97 ships with.
        bool ParseBin(const std::filesystem::path& a_path, std::string& a_diag) {
            std::ifstream f(a_path, std::ios::binary);
            if (!f) {
                a_diag = "open failed";
                return false;
            }
            std::vector<unsigned char> data(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            if (data.size() < 24) {
                a_diag = "file too small";
                return false;
            }

            std::size_t pos = 0;
            auto u8  = [&]() { return data[pos++]; };
            auto u16 = [&]() { std::uint16_t v; std::memcpy(&v, data.data()+pos, 2); pos += 2; return v; };
            auto u32 = [&]() { std::uint32_t v; std::memcpy(&v, data.data()+pos, 4); pos += 4; return v; };
            auto u64 = [&]() { std::uint64_t v; std::memcpy(&v, data.data()+pos, 8); pos += 8; return v; };

            const std::uint32_t fmt = u32();
            if (fmt != 1) {
                a_diag = std::format("unexpected format {}", fmt);
                return false;
            }
            (void)u32(); (void)u32(); (void)u32(); (void)u32();   // version quad
            const std::uint32_t nameLen = u32();
            if (pos + nameLen > data.size()) {
                a_diag = "name overflow";
                return false;
            }
            pos += nameLen;
            const std::uint32_t ptrSize = u32();
            const std::uint32_t count   = u32();
            if (ptrSize == 0) {
                a_diag = "ptrSize=0";
                return false;
            }

            g_entries.clear();
            g_entries.reserve(count);

            std::uint64_t prevId  = 0;
            std::uint64_t prevOff = 0;

            for (std::uint32_t i = 0; i < count; ++i) {
                if (pos >= data.size()) { a_diag = "truncated"; return false; }
                const std::uint8_t t  = u8();
                const std::uint8_t lo = t & 0x0F;
                const std::uint8_t hi = (t >> 4) & 0x0F;

                std::uint64_t curId = 0;
                switch (lo) {
                case 0: curId = u64(); break;
                case 1: curId = prevId + 1; break;
                case 2: curId = prevId + u8();  break;
                case 3: curId = prevId - u8();  break;
                case 4: curId = prevId + u16(); break;
                case 5: curId = prevId - u16(); break;
                case 6: curId = u16(); break;
                case 7: curId = u32(); break;
                default:
                    a_diag = std::format("bad id_method {} at i={}", lo, i);
                    return false;
                }

                const std::uint8_t method = hi & 7;
                const bool         scaled = (hi & 8) != 0;
                const std::uint64_t tmp   = scaled ? (prevOff / ptrSize) : prevOff;

                std::uint64_t curOff = 0;
                switch (method) {
                case 0: curOff = u64(); break;
                case 1: curOff = tmp + 1; break;
                case 2: curOff = tmp + u8();  break;
                case 3: curOff = tmp - u8();  break;
                case 4: curOff = tmp + u16(); break;
                case 5: curOff = tmp - u16(); break;
                case 6: curOff = u16(); break;
                case 7: curOff = u32(); break;
                }
                if (scaled) curOff *= ptrSize;

                g_entries.push_back(Entry{
                    static_cast<std::uintptr_t>(curOff),
                    static_cast<std::uint32_t>(curId)});

                prevId  = curId;
                prevOff = curOff;
            }

            // Sort by RVA so we can do a binary-search nearest-below.
            std::sort(g_entries.begin(), g_entries.end(),
                      [](const Entry& l, const Entry& r){ return l.rva < r.rva; });

            a_diag = std::format("loaded {} entries from {}", g_entries.size(),
                                 a_path.string());
            return true;
        }

    }

    void Init() {
        const auto h = ::GetModuleHandleW(L"SkyrimSE.exe");
        g_skyrimBase = h ? reinterpret_cast<std::uintptr_t>(h) : 0;

        const auto path = FindAddrLibBin();
        if (path.empty()) {
            g_diagnostic = "version-1-5-97-0.bin not found";
            logs::warn("AddrLib::Init - {}", g_diagnostic);
            return;
        }
        std::string diag;
        const bool ok = ParseBin(path, diag);
        g_diagnostic = std::move(diag);
        g_available  = ok;
        if (ok) {
            logs::info("AddrLib::Init - {}", g_diagnostic);
        } else {
            logs::warn("AddrLib::Init - parse failed: {}", g_diagnostic);
        }
    }

    bool Available() noexcept { return g_available.load(std::memory_order_relaxed); }

    std::string DiagnosticString() { return g_diagnostic; }

    Hit Resolve(std::uintptr_t a_addr) noexcept {
        Hit h;
        if (!Available() || g_skyrimBase == 0) return h;
        if (a_addr < g_skyrimBase) return h;
        const auto rva = a_addr - g_skyrimBase;

        // Binary search for the largest entry with rva <= target.
        // std::upper_bound returns the first strictly greater entry; step
        // back one for the predecessor.
        Entry probe{rva, 0};
        auto it = std::upper_bound(
            g_entries.begin(), g_entries.end(), probe,
            [](const Entry& l, const Entry& r){ return l.rva < r.rva; });
        if (it == g_entries.begin()) return h;
        --it;

        h.found    = true;
        h.id       = it->id;
        h.base_rva = it->rva;
        h.delta    = static_cast<std::int64_t>(rva) - static_cast<std::int64_t>(it->rva);
        return h;
    }

    std::string FormatAnnotation(std::uintptr_t a_addr) {
        const auto h = Resolve(a_addr);
        if (!h.found) return {};
        // 256 bytes of slack is large but a few outlier funcs are big;
        // beyond that the "nearest ID" annotation is misleading rather
        // than helpful, so we drop it to keep the report honest.
        if (h.delta < 0 || h.delta > 0x4000) return {};
        return std::format("[id {} +0x{:x}]", h.id, h.delta);
    }

}
