#include "PCH.h"
#include "SkyrimAnchors.h"

#include <Windows.h>

#include <atomic>

namespace FreezeLogger::SkyrimAnchors {

    namespace {

        Anchors           g_anchors{};
        std::atomic<bool> g_done{false};
        std::string       g_diag{"<not initialised>"};

        // WaitForJobTask body signature (see header). 15 bytes, no wildcards.
        constexpr unsigned char kSig[] = {
            0x44, 0x8B, 0xC1,             // mov  r8d, ecx
            0x48, 0x8B, 0x48, 0x08,       // mov  rcx, [rax+8]
            0x4A, 0x8B, 0x0C, 0xC1,       // mov  rcx, [rcx+r8*8]
            0x48, 0x85, 0xC9,             // test rcx, rcx
            0x74,                         // je   short
        };
        constexpr std::size_t kSigLen = sizeof(kSig);

        // mov rax, [rip+disp32]  =>  48 8B 05 <disp32>
        constexpr unsigned char kMovRaxRip[] = {0x48, 0x8B, 0x05};

        // Locate the executable .text section of an in-memory PE image.
        // Returns false on any malformed header (defensive; the game image
        // is always well-formed, but we never trust raw memory blindly).
        bool FindTextSection(std::uintptr_t a_base,
                             std::uintptr_t& a_textVA,
                             std::uintptr_t& a_textSize,
                             std::uintptr_t& a_imageSize) noexcept
        {
            __try {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(a_base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
                    a_base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

                a_imageSize = nt->OptionalHeader.SizeOfImage;

                const auto* sec = IMAGE_FIRST_SECTION(nt);
                const auto  n   = nt->FileHeader.NumberOfSections;
                for (unsigned i = 0; i < n; ++i) {
                    const auto& s = sec[i];
                    const bool isText =
                        std::memcmp(s.Name, ".text", 5) == 0;
                    const bool isExec =
                        (s.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
                    if (isText && isExec) {
                        a_textVA   = a_base + s.VirtualAddress;
                        a_textSize = s.Misc.VirtualSize;
                        return true;
                    }
                }
                return false;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Forward scan for the body signature inside [a_start, a_start+a_len).
        // Returns the VA of the first match, or 0. SEH-guarded: executable
        // memory is committed, but a torn section header could mislead us.
        std::uintptr_t ScanSignature(std::uintptr_t a_start,
                                     std::uintptr_t a_len) noexcept
        {
            __try {
                const auto* p   = reinterpret_cast<const unsigned char*>(a_start);
                const auto  end = (a_len > kSigLen) ? (a_len - kSigLen) : 0;
                for (std::uintptr_t i = 0; i < end; ++i) {
                    if (p[i] == kSig[0] &&
                        std::memcmp(p + i, kSig, kSigLen) == 0) {
                        return a_start + i;
                    }
                }
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // From the matched body VA, walk back up to 16 bytes for the
        // `mov rax,[rip+disp32]` prologue and decode its target slot.
        bool DerivePrologue(std::uintptr_t a_bodyVA,
                            std::uintptr_t& a_fnVA,
                            std::uintptr_t& a_slotVA) noexcept
        {
            __try {
                const auto* base = reinterpret_cast<const unsigned char*>(a_bodyVA - 16);
                for (int j = 16 - 3; j >= 0; --j) {
                    if (std::memcmp(base + j, kMovRaxRip, sizeof(kMovRaxRip)) == 0) {
                        const std::uintptr_t movVA = (a_bodyVA - 16) + j;
                        std::int32_t disp = 0;
                        std::memcpy(&disp, reinterpret_cast<const void*>(movVA + 3),
                                    sizeof(disp));
                        // mov rax,[rip+disp32] is 7 bytes; rip = next insn.
                        a_fnVA   = movVA;
                        a_slotVA = movVA + 7 + static_cast<std::uintptr_t>(
                                       static_cast<std::intptr_t>(disp));
                        return true;
                    }
                }
                return false;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

    }   // anonymous

    void Init() {
        if (g_done.exchange(true)) return;

        // GetModuleHandle(nullptr) = the process's main module, i.e. the
        // game EXE (SkyrimSE.exe / SkyrimVR.exe) regardless of runtime.
        const HMODULE h = ::GetModuleHandleW(nullptr);
        if (!h) {
            g_diag = "GetModuleHandle(nullptr) failed";
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(h);
        g_anchors.moduleBase = base;

        static wchar_t nameBuf[MAX_PATH] = {};
        if (::GetModuleFileNameW(h, nameBuf, MAX_PATH) != 0) {
            // Trim to the bare filename for the report.
            const wchar_t* slash = nameBuf;
            for (const wchar_t* p = nameBuf; *p; ++p) {
                if (*p == L'\\' || *p == L'/') slash = p + 1;
            }
            g_anchors.moduleName = slash;
        }

        std::uintptr_t textVA = 0, textSize = 0, imageSize = 0;
        if (!FindTextSection(base, textVA, textSize, imageSize)) {
            g_diag = "could not locate .text section";
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
            return;
        }
        g_anchors.moduleSize = imageSize;

        const auto bodyVA = ScanSignature(textVA, textSize);
        if (bodyVA == 0) {
            g_diag = std::format(
                "WaitForJobTask body signature not found in .text "
                "(base 0x{:x}, .text 0x{:x}+0x{:x}); deep Site-B probe disabled "
                "on this runtime",
                base, textVA, textSize);
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
            return;
        }

        std::uintptr_t fnVA = 0, slotVA = 0;
        if (!DerivePrologue(bodyVA, fnVA, slotVA) || slotVA == 0) {
            g_diag = std::format(
                "found body @0x{:x} but could not decode mov rax,[rip] prologue",
                bodyVA);
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
            return;
        }

        g_anchors.waitForJobTask    = fnVA;
        g_anchors.singletonBSlot    = slotVA;
        g_anchors.waitForJobTaskRVA = fnVA - base;
        g_anchors.singletonBSlotRVA = slotVA - base;
        g_anchors.resolved          = true;

        g_diag = std::format(
            "resolved: WaitForJobTask @0x{:x} (+0x{:x}), Singleton-B slot "
            "@0x{:x} (+0x{:x}) in {}",
            fnVA, g_anchors.waitForJobTaskRVA,
            slotVA, g_anchors.singletonBSlotRVA,
            "module");
        logs::info("SkyrimAnchors::Init - {}", g_diag);
    }

    const Anchors& Get() noexcept { return g_anchors; }

    bool Available() noexcept {
        return g_anchors.resolved && g_anchors.singletonBSlot != 0;
    }

    const char* DiagnosticString() { return g_diag.c_str(); }

}
