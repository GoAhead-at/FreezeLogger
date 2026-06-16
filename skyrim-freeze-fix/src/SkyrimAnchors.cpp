#include "PCH.h"
#include "SkyrimAnchors.h"

#include <format>

namespace WorkerSpinLockFix::SkyrimAnchors {

    namespace {

        Anchors           g_anchors{};
        std::atomic<bool> g_done{ false };
        std::string       g_diag{ "<not initialised>" };
        std::string       g_diagA{ "<not initialised>" };

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
        constexpr unsigned char kMovRaxRip[] = { 0x48, 0x8B, 0x05 };

        // ----- Site-A (id 34554) body signature -------------------------
        // Anchored at the `test rbx,rbx` that immediately follows the
        // singleton load. 0x00 entries are wildcards (the two short-jump
        // displacements, which vary if the surrounding code is recompiled):
        //
        //   48 85 DB              test rbx, rbx
        //   74 ??                 je   short            (bail if null)
        //   83 7B 6C 01           cmp  dword [rbx+0x6c], 1   (pending?)
        //   75 ??                 jne  short            (bail if not pending)
        //   48 8B 4B 60           mov  rcx, [rbx+0x60]  (worker-ack HANDLE)
        //   45 33 C0              xor  r8d, r8d         (bAlertable=FALSE)
        //   83 CA FF              or   edx, 0xffffffff  (INFINITE)
        //   C7 43 68 00 00 00 00  mov  dword [rbx+0x68], 0  (work-id=0)
        //
        // The cmp[rbx+0x6c],1 / mov rcx,[rbx+0x60] / mov[rbx+0x68],0 triple
        // is effectively unique in the binary. The `mov rbx,[rip+disp32]`
        // singleton load sits in the 7 bytes immediately before the match,
        // from which we derive the Singleton-A slot.
        constexpr unsigned char kSiteASig[] = {
            0x48, 0x85, 0xDB,                   // test rbx, rbx
            0x74, 0x00,                         // je   short   (wildcard)
            0x83, 0x7B, 0x6C, 0x01,             // cmp  dword [rbx+0x6c], 1
            0x75, 0x00,                         // jne  short   (wildcard)
            0x48, 0x8B, 0x4B, 0x60,             // mov  rcx, [rbx+0x60]
            0x45, 0x33, 0xC0,                   // xor  r8d, r8d
            0x83, 0xCA, 0xFF,                   // or   edx, 0xffffffff
            0xC7, 0x43, 0x68, 0x00, 0x00, 0x00, 0x00,  // mov dword [rbx+0x68],0
        };
        // Wildcard mask: 1 == compare, 0 == ignore this byte.
        constexpr unsigned char kSiteAMask[] = {
            1, 1, 1,
            1, 0,
            1, 1, 1, 1,
            1, 0,
            1, 1, 1, 1,
            1, 1, 1,
            1, 1, 1,
            1, 1, 1, 1, 1, 1, 1,
        };
        constexpr std::size_t kSiteASigLen = sizeof(kSiteASig);
        static_assert(sizeof(kSiteAMask) == sizeof(kSiteASig),
            "Site-A sig/mask length mismatch");

        // mov rbx, [rip+disp32]  =>  48 8B 1D <disp32>
        constexpr unsigned char kMovRbxRip[] = { 0x48, 0x8B, 0x1D };
        // sub rsp, 0x20          =>  48 83 EC 20
        constexpr unsigned char kSubRsp20[]  = { 0x48, 0x83, 0xEC, 0x20 };

        // Locate the executable .text section of an in-memory PE image.
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
                    const bool isText = std::memcmp(s.Name, ".text", 5) == 0;
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

        // Masked forward scan for the Site-A body signature inside
        // [a_start, a_start+a_len). Returns the VA of the first match (the
        // `test rbx,rbx`) or 0. SEH-guarded like ScanSignature.
        std::uintptr_t ScanSiteASignature(std::uintptr_t a_start,
                                          std::uintptr_t a_len) noexcept
        {
            __try {
                const auto* p   = reinterpret_cast<const unsigned char*>(a_start);
                const auto  end = (a_len > kSiteASigLen) ? (a_len - kSiteASigLen) : 0;
                for (std::uintptr_t i = 0; i < end; ++i) {
                    if (p[i] != kSiteASig[0]) continue;
                    bool hit = true;
                    for (std::size_t j = 1; j < kSiteASigLen; ++j) {
                        if (kSiteAMask[j] && p[i + j] != kSiteASig[j]) {
                            hit = false;
                            break;
                        }
                    }
                    if (hit) return a_start + i;
                }
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // From the Site-A body match (the `test rbx,rbx` VA), decode the
        // `mov rbx,[rip+disp32]` in the 7 bytes immediately before it to
        // get Singleton-A's slot, and walk the prologue backward to find
        // the function entry (the `push rbx` -- 1 or 2 bytes -- that
        // precedes `sub rsp,0x20`). Returns false on any mismatch so the
        // SiteABreaker stays idle rather than hooking the wrong address.
        bool DeriveSiteA(std::uintptr_t  a_bodyVA,
                         std::uintptr_t& a_fnVA,
                         std::uintptr_t& a_slotVA) noexcept
        {
            __try {
                const auto movVA = a_bodyVA - 7;
                if (std::memcmp(reinterpret_cast<const void*>(movVA),
                                kMovRbxRip, sizeof(kMovRbxRip)) != 0) {
                    return false;
                }
                std::int32_t disp = 0;
                std::memcpy(&disp, reinterpret_cast<const void*>(movVA + 3),
                            sizeof(disp));
                // rip = end of the 7-byte mov = a_bodyVA.
                a_slotVA = a_bodyVA + static_cast<std::uintptr_t>(
                                          static_cast<std::intptr_t>(disp));

                // `sub rsp,0x20` must sit in the 4 bytes before the mov.
                const auto subVA = movVA - 4;
                if (std::memcmp(reinterpret_cast<const void*>(subVA),
                                kSubRsp20, sizeof(kSubRsp20)) != 0) {
                    return false;
                }
                // `push rbx`: 0x53, optionally with a redundant 0x40 REX.
                const auto* b = reinterpret_cast<const unsigned char*>(subVA);
                if (b[-1] == 0x53 && b[-2] == 0x40) {
                    a_fnVA = subVA - 2;
                } else if (b[-1] == 0x53) {
                    a_fnVA = subVA - 1;
                } else {
                    return false;
                }
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

    }   // anonymous

    void Init() {
        if (g_done.exchange(true)) return;

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

        // ----- Site-A (id 34554) -- resolved independently of Site-B ----
        // A Site-B failure must not prevent Site-A from arming and vice
        // versa, so this runs before the Site-B early-returns below.
        {
            const auto bodyA = ScanSiteASignature(textVA, textSize);
            if (bodyA == 0) {
                g_diagA = "Site-A (id 34554) body signature not found in "
                          ".text; SiteABreaker disabled on this runtime";
                logs::warn("SkyrimAnchors::Init - {}", g_diagA);
            } else {
                std::uintptr_t fnA = 0, slotA = 0;
                if (!DeriveSiteA(bodyA, fnA, slotA) || fnA == 0 || slotA == 0) {
                    g_diagA = std::format(
                        "Site-A body @0x{:x} but could not decode the "
                        "mov rbx,[rip] / prologue; SiteABreaker disabled",
                        bodyA);
                    logs::warn("SkyrimAnchors::Init - {}", g_diagA);
                } else {
                    g_anchors.siteALockFn       = fnA;
                    g_anchors.singletonASlot    = slotA;
                    g_anchors.siteALockFnRVA    = fnA - base;
                    g_anchors.singletonASlotRVA = slotA - base;
                    g_anchors.siteAResolved     = true;
                    g_diagA = std::format(
                        "resolved: Site-A id 34554 @0x{:x} (+0x{:x}), "
                        "Singleton-A slot @0x{:x} (+0x{:x})",
                        fnA, g_anchors.siteALockFnRVA,
                        slotA, g_anchors.singletonASlotRVA);
                    logs::info("SkyrimAnchors::Init - {}", g_diagA);
                }
            }
        }

        const auto bodyVA = ScanSignature(textVA, textSize);
        if (bodyVA == 0) {
            g_diag = std::format(
                "WaitForJobTask body signature not found in .text "
                "(base 0x{:x}, .text 0x{:x}+0x{:x}); JobWaitBreaker disabled "
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
            "@0x{:x} (+0x{:x})",
            fnVA, g_anchors.waitForJobTaskRVA,
            slotVA, g_anchors.singletonBSlotRVA);
        logs::info("SkyrimAnchors::Init - {}", g_diag);
    }

    const Anchors& Get() noexcept { return g_anchors; }

    bool Available() noexcept {
        return g_anchors.resolved && g_anchors.singletonBSlot != 0;
    }

    bool AvailableSiteA() noexcept {
        return g_anchors.siteAResolved && g_anchors.siteALockFn != 0 &&
               g_anchors.singletonASlot != 0;
    }

    const char* DiagnosticString() { return g_diag.c_str(); }

    const char* DiagnosticStringSiteA() { return g_diagA.c_str(); }

}
