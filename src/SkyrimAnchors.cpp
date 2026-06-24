#include "PCH.h"
#include "SkyrimAnchors.h"

#include <Windows.h>

#include <atomic>

namespace FreezeLogger::SkyrimAnchors {

    namespace {

        Anchors           g_anchors{};
        std::atomic<bool> g_done{false};
        std::string       g_diag{"<not initialised>"};
        std::string       g_diagSiteA{"<not initialised>"};

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

        // Site-A lock primitive body (version-stable core; see header). The
        // two rel8 jump targets are wildcarded (mask byte == 1).
        //   test rbx,rbx / je * / cmp [rbx+0x6c],1 / jne * / mov rcx,[rbx+0x60]
        constexpr unsigned char kSiteASig[] = {
            0x48, 0x85, 0xDB,                // test rbx, rbx
            0x74, 0x00,                      // je   short (rel8 *)
            0x83, 0x7B, 0x6C, 0x01,          // cmp  dword [rbx+0x6c], 1
            0x75, 0x00,                      // jne  short (rel8 *)
            0x48, 0x8B, 0x4B, 0x60,          // mov  rcx, [rbx+0x60]
        };
        constexpr unsigned char kSiteAMask[] = {
            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0,
        };
        constexpr std::size_t kSiteASigLen = sizeof(kSiteASig);

        // mov rbx, [rip+disp32]  =>  48 8B 1D <disp32>  (Singleton-A load)
        constexpr unsigned char kMovRbxRip[] = {0x48, 0x8B, 0x1D};
        // push rbx; sub rsp,0x20  preamble immediately before kMovRbxRip
        constexpr unsigned char kSiteAPreamble[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20};

        // Render-side Site-A join function (SE id 34557) entry signature.
        // Byte-unique in .text (verified against the unpacked SE 1.5.97
        // binary): the `mov byte [rcx+0x71],1` flag write right after the
        // 3-push / sub-rsp-0x30 prologue is the distinctive tail.
        //   push rbx; push rsi; push rdi; sub rsp,0x30; mov byte [rcx+0x71],1
        constexpr unsigned char kRenderTaskSig[] = {
            0x40, 0x53,                      // push rbx
            0x56,                            // push rsi
            0x57,                            // push rdi
            0x48, 0x83, 0xEC, 0x30,          // sub  rsp, 0x30
            0xC6, 0x41, 0x71, 0x01,          // mov  byte [rcx+0x71], 1
        };
        constexpr std::size_t kRenderTaskSigLen = sizeof(kRenderTaskSig);

        // BSSpinLock spin-retry yield call (version-stable; only the call
        // displacement differs across builds, so bytes 6..9 are wildcarded).
        //   inc ebx / xor ecx,ecx / call [rip+d32] / xor eax,eax / lock cmpxchg
        // The instruction after the call (xor eax,eax) is the spin-retry
        // return address threads spinning on a lock park above.
        constexpr unsigned char kSpinSig[] = {
            0xFF, 0xC3,                      // inc  ebx
            0x33, 0xC9,                      // xor  ecx, ecx
            0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,  // call [rip+d32] (*)
            0x33, 0xC0,                      // xor  eax, eax  <- spin-retry ret
            0xF0, 0x44, 0x0F, 0xB1, 0x77, 0x04,  // lock cmpxchg [rdi+4], r14d
        };
        constexpr unsigned char kSpinMask[] = {
            0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        constexpr std::size_t kSpinSigLen = sizeof(kSpinSig);
        constexpr std::size_t kSpinRetOff = 10;   // xor eax,eax within kSpinSig

        // How far either side of the Site-A call site to look for the
        // Main::Update WaitForJobTask calls (they sit within ~0x900 on both
        // SE and AE; 0x1000 is a safe window that still excludes the other
        // WaitForJobTask callers elsewhere in the binary).
        constexpr std::uintptr_t kMainUpdateProximity = 0x1000;

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

        // True iff the masked signature matches the bytes at a_p. A null
        // mask means "every byte is significant" (no wildcards).
        bool MatchMasked(const unsigned char* a_p,
                         const unsigned char* a_sig,
                         const unsigned char* a_mask,
                         std::size_t          a_len) noexcept
        {
            for (std::size_t i = 0; i < a_len; ++i) {
                if ((!a_mask || !a_mask[i]) && a_p[i] != a_sig[i]) return false;
            }
            return true;
        }

        // Forward scan for the first masked-signature match in [start,start+len).
        std::uintptr_t ScanMaskedFirst(std::uintptr_t       a_start,
                                       std::uintptr_t       a_len,
                                       const unsigned char* a_sig,
                                       const unsigned char* a_mask,
                                       std::size_t          a_sigLen) noexcept
        {
            __try {
                const auto* p   = reinterpret_cast<const unsigned char*>(a_start);
                const auto  end = (a_len > a_sigLen) ? (a_len - a_sigLen) : 0;
                for (std::uintptr_t i = 0; i < end; ++i) {
                    if (p[i] == a_sig[0] &&
                        MatchMasked(p + i, a_sig, a_mask, a_sigLen)) {
                        return a_start + i;
                    }
                }
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // Collect every masked-signature match VA into a_out (capped). Returns
        // the number stored.
        std::size_t ScanMaskedAll(std::uintptr_t       a_start,
                                  std::uintptr_t       a_len,
                                  const unsigned char* a_sig,
                                  const unsigned char* a_mask,
                                  std::size_t          a_sigLen,
                                  std::uintptr_t*      a_out,
                                  std::size_t          a_cap) noexcept
        {
            std::size_t n = 0;
            __try {
                const auto* p   = reinterpret_cast<const unsigned char*>(a_start);
                const auto  end = (a_len > a_sigLen) ? (a_len - a_sigLen) : 0;
                for (std::uintptr_t i = 0; i < end && n < a_cap; ++i) {
                    if (p[i] == a_sig[0] &&
                        MatchMasked(p + i, a_sig, a_mask, a_sigLen)) {
                        a_out[n++] = a_start + i;
                    }
                }
                return n;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return n;
            }
        }

        // Walk back up to 16 bytes from a_bodyVA for `mov rbx,[rip+disp32]`
        // (48 8B 1D) and decode its target slot + the function entry (the
        // push-rbx/sub-rsp preamble that precedes the mov).
        bool DeriveSiteAPrologue(std::uintptr_t  a_bodyVA,
                                 std::uintptr_t& a_fnEntry,
                                 std::uintptr_t& a_slotVA) noexcept
        {
            __try {
                for (int back = 3; back <= 16; ++back) {
                    const auto movVA = a_bodyVA - static_cast<std::uintptr_t>(back);
                    if (std::memcmp(reinterpret_cast<const void*>(movVA),
                                    kMovRbxRip, sizeof(kMovRbxRip)) == 0) {
                        std::int32_t disp = 0;
                        std::memcpy(&disp, reinterpret_cast<const void*>(movVA + 3),
                                    sizeof(disp));
                        a_slotVA = movVA + 7 + static_cast<std::uintptr_t>(
                                       static_cast<std::intptr_t>(disp));
                        const auto preVA = movVA - sizeof(kSiteAPreamble);
                        a_fnEntry =
                            (std::memcmp(reinterpret_cast<const void*>(preVA),
                                         kSiteAPreamble, sizeof(kSiteAPreamble)) == 0)
                                ? preVA
                                : movVA;  // fall back to the mov if padding moved
                        return true;
                    }
                }
                return false;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Walk forward up to a_window bytes from a_from for `call [rip+d32]`
        // (FF 15) and return the address of the instruction after it.
        std::uintptr_t FindCallReturnForward(std::uintptr_t a_from,
                                             std::uintptr_t a_window) noexcept
        {
            __try {
                const auto* p = reinterpret_cast<const unsigned char*>(a_from);
                for (std::uintptr_t i = 0; i < a_window; ++i) {
                    if (p[i] == 0xFF && p[i + 1] == 0x15) {
                        return a_from + i + 6;  // FF 15 <disp32> = 6 bytes
                    }
                }
                return 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }

        // Walk forward from a_entry for the function's terminating `ret`
        // (0xC3) that is immediately followed by an int3 (0xCC) alignment
        // pad -- the standard MSVC function boundary. Returns the VA just
        // past that ret (exclusive end), or a_entry + a_cap if no such
        // boundary is found within the cap. SEH-guarded.
        std::uintptr_t FindFunctionEnd(std::uintptr_t a_entry,
                                       std::uintptr_t a_cap) noexcept
        {
            __try {
                const auto* p = reinterpret_cast<const unsigned char*>(a_entry);
                for (std::uintptr_t i = 0; i + 1 < a_cap; ++i) {
                    if (p[i] == 0xC3 && p[i + 1] == 0xCC) {
                        return a_entry + i + 1;
                    }
                }
                return a_entry + a_cap;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return a_entry + a_cap;
            }
        }

        // Scan [start,start+len) for `call rel32` (E8) sites whose target ==
        // a_target. Stores each call site VA in a_outSites (capped); returns
        // the count. The return address is the site VA + 5.
        std::size_t ScanRel32CallSites(std::uintptr_t  a_start,
                                       std::uintptr_t  a_len,
                                       std::uintptr_t  a_target,
                                       std::uintptr_t* a_outSites,
                                       std::size_t     a_cap) noexcept
        {
            std::size_t n = 0;
            __try {
                const auto* p = reinterpret_cast<const unsigned char*>(a_start);
                const auto  end = (a_len > 5) ? (a_len - 5) : 0;
                for (std::uintptr_t i = 0; i < end && n < a_cap; ) {
                    if (p[i] == 0xE8) {
                        std::int32_t rel = 0;
                        std::memcpy(&rel, p + i + 1, sizeof(rel));
                        const auto site = a_start + i;
                        const auto tgt  = site + 5 +
                            static_cast<std::uintptr_t>(static_cast<std::intptr_t>(rel));
                        if (tgt == a_target) a_outSites[n++] = site;
                        i += 5;
                    } else {
                        i += 1;
                    }
                }
                return n;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return n;
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

        // ===== Site-B: WaitForJobTask + Singleton-B (non-fatal) ==========
        // A failure here disables only the Site-B probe; Site-A and the
        // spinlock search below resolve independently.
        const auto bodyVA = ScanSignature(textVA, textSize);
        std::uintptr_t fnVA = 0, slotVA = 0;
        if (bodyVA == 0) {
            g_diag = std::format(
                "WaitForJobTask body signature not found in .text "
                "(base 0x{:x}, .text 0x{:x}+0x{:x}); Site-B probe disabled "
                "on this runtime",
                base, textVA, textSize);
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
        } else if (!DerivePrologue(bodyVA, fnVA, slotVA) || slotVA == 0) {
            g_diag = std::format(
                "found WaitForJobTask body @0x{:x} but could not decode "
                "mov rax,[rip] prologue", bodyVA);
            logs::warn("SkyrimAnchors::Init - {}", g_diag);
        } else {
            g_anchors.waitForJobTask    = fnVA;
            g_anchors.singletonBSlot    = slotVA;
            g_anchors.waitForJobTaskRVA = fnVA - base;
            g_anchors.singletonBSlotRVA = slotVA - base;
            g_anchors.resolved          = true;
            g_diag = std::format(
                "resolved: WaitForJobTask @0x{:x} (+0x{:x}), Singleton-B slot "
                "@0x{:x} (+0x{:x}) in module",
                fnVA, g_anchors.waitForJobTaskRVA,
                slotVA, g_anchors.singletonBSlotRVA);
            logs::info("SkyrimAnchors::Init - {}", g_diag);
        }

        // ===== Site-A: lock primitive (id 34554) + Singleton-A ===========
        const auto siteABody =
            ScanMaskedFirst(textVA, textSize, kSiteASig, kSiteAMask, kSiteASigLen);
        if (siteABody != 0) {
            std::uintptr_t entry = 0, aslot = 0;
            const bool gotPrologue = DeriveSiteAPrologue(siteABody, entry, aslot);
            const auto lockRet =
                FindCallReturnForward(siteABody + kSiteASigLen, kSiteASigLen + 0x10);
            if (gotPrologue && aslot != 0 && lockRet != 0) {
                g_anchors.siteALockFn        = entry;
                g_anchors.siteALockReturn    = lockRet;
                g_anchors.siteALockFnEnd     = lockRet + 0x20;
                g_anchors.singletonASlot     = aslot;
                g_anchors.siteALockFnRVA     = entry - base;
                g_anchors.siteALockReturnRVA = lockRet - base;
                g_anchors.singletonASlotRVA  = aslot - base;
                g_anchors.siteAResolved      = true;

                // Main::Update return addresses, resolved from anchors only.
                std::uintptr_t callA[2] = {0, 0};
                const auto nA = ScanRel32CallSites(
                    textVA, textSize, entry, callA, 2);
                if (nA >= 1) {
                    g_anchors.mainUpdateRetA = callA[0] + 5;
                    // WaitForJobTask call sites near the Site-A call site are
                    // the Main::Update Site-B waits (2 on SE and AE).
                    if (g_anchors.waitForJobTask != 0) {
                        const auto lo = (callA[0] > textVA + kMainUpdateProximity)
                                            ? callA[0] - kMainUpdateProximity
                                            : textVA;
                        const auto hi = callA[0] + kMainUpdateProximity;
                        const auto winLen = (hi > lo) ? (hi - lo) : 0;
                        std::uintptr_t callB[4] = {0, 0, 0, 0};
                        const auto nB = ScanRel32CallSites(
                            lo, winLen, g_anchors.waitForJobTask, callB, 4);
                        for (std::size_t i = 0; i < nB; ++i) {
                            g_anchors.mainUpdateRetB[g_anchors.mainUpdateRetBCount++] =
                                callB[i] + 5;
                        }
                    }
                }
            }
        }

        // ===== BSSpinLock spin-retry sites (1 on SE, 2 on AE) ============
        g_anchors.spinRetCount = ScanMaskedAll(
            textVA, textSize, kSpinSig, kSpinMask, kSpinSigLen,
            g_anchors.spinRetAddr, 4);
        for (std::size_t i = 0; i < g_anchors.spinRetCount; ++i) {
            // Stored as the return address (xor eax,eax after the yield call).
            g_anchors.spinRetAddr[i] += kSpinRetOff;
        }
        g_anchors.spinResolved = g_anchors.spinRetCount > 0;

        // ===== Render-side Site-A join function (id 34557) ===============
        // Independent of the main-side Site-A: a missing render anchor must
        // not disturb the main-side probe, and vice versa.
        const auto renderTaskVA = ScanMaskedFirst(
            textVA, textSize, kRenderTaskSig, /*mask=*/nullptr, kRenderTaskSigLen);
        if (renderTaskVA != 0) {
            g_anchors.renderTaskFn        = renderTaskVA;
            g_anchors.renderTaskFnEnd     = FindFunctionEnd(renderTaskVA, 0x800);
            g_anchors.renderTaskFnRVA     = renderTaskVA - base;
            g_anchors.renderSiteAResolved = true;
        }

        if (g_anchors.siteAResolved) {
            g_diagSiteA = std::format(
                "resolved: Site-A lock fn @0x{:x} (+0x{:x}), lock-return +0x{:x}, "
                "Singleton-A slot +0x{:x}; Main::Update retA={}, retB x{}; "
                "spin-retry sites x{}; render-Site-A(id 34557)={}",
                g_anchors.siteALockFn, g_anchors.siteALockFnRVA,
                g_anchors.siteALockReturnRVA, g_anchors.singletonASlotRVA,
                g_anchors.mainUpdateRetA != 0 ? "yes" : "no",
                g_anchors.mainUpdateRetBCount, g_anchors.spinRetCount,
                g_anchors.renderSiteAResolved
                    ? std::format("@0x{:x} (+0x{:x}, len 0x{:x})",
                                  g_anchors.renderTaskFn, g_anchors.renderTaskFnRVA,
                                  g_anchors.renderTaskFnEnd - g_anchors.renderTaskFn)
                    : std::string{"not found"});
            logs::info("SkyrimAnchors::Init - {}", g_diagSiteA);
        } else {
            g_diagSiteA = std::format(
                "Site-A lock primitive signature not found in .text; "
                "long-form Site-A probe disabled on this runtime "
                "(spin-retry sites x{})",
                g_anchors.spinRetCount);
            logs::warn("SkyrimAnchors::Init - {}", g_diagSiteA);
        }
    }

    const Anchors& Get() noexcept { return g_anchors; }

    bool Available() noexcept {
        return g_anchors.resolved && g_anchors.singletonBSlot != 0;
    }

    bool SiteAAvailable() noexcept {
        return g_anchors.siteAResolved && g_anchors.singletonASlot != 0;
    }

    bool SpinAvailable() noexcept {
        return g_anchors.spinResolved && g_anchors.spinRetCount > 0;
    }

    bool RenderSiteAAvailable() noexcept {
        return g_anchors.renderSiteAResolved && g_anchors.renderTaskFn != 0 &&
               g_anchors.renderTaskFnEnd > g_anchors.renderTaskFn;
    }

    const char* DiagnosticString() { return g_diag.c_str(); }

    const char* SiteADiagnosticString() { return g_diagSiteA.c_str(); }

}
