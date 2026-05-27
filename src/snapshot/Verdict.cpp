#include "PCH.h"
#include "snapshot/Verdict.h"

#include "Heartbeat.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cctype>
#include <vector>

namespace FreezeLogger::Snapshot::Verdict {

    // ====================================================================
    // RVA constants — must stay in sync with MainWaitProbe.cpp. These are
    // the same site-detection anchors used in the long-form audit; we
    // re-evaluate them here so Verdict stays the "cheap, runs first"
    // probe and doesn't depend on MainWaitProbe's internal state.
    // ====================================================================
    namespace {
        // Site A — id 34554 lock primitive.
        constexpr std::uintptr_t kLockFnLoRVA   = 0x5765d0;
        constexpr std::uintptr_t kLockFnHiRVA   = 0x576620;
        constexpr std::uintptr_t kLockReturnRVA = 0x5765ff;

        // Site B — +0xc38130 event-source wrapper.
        constexpr std::uintptr_t kSingletonBPtrRVA  = 0x2f26a70;
        constexpr std::uintptr_t kWaitWrapperLoRVA  = 0xc38130;
        constexpr std::uintptr_t kWaitWrapperHiRVA  = 0xc3815b;
        constexpr std::uintptr_t kMainUpdateRetBRVA = 0x5b34fe;

        // BSSpinLock spin-retry RVA inside +0x132bd0.
        constexpr std::uintptr_t kSpinRetRVA = 0x132c5a;

        // Stack-scan windows. The 2 KiB window for site detection mirrors
        // MainWaitProbe; the 256-byte window for the spin-retry scan is
        // a tight bound on KERNELBASE!SleepEx's frame.
        constexpr std::size_t kSiteScanWindow    = 0x400;  // 1 KiB
        constexpr std::size_t kHdtsmpScanWindow  = 0x800;  // 2 KiB
        constexpr std::size_t kSpinScanWindow    = 0x100;  // 256 B
    }

    // ====================================================================
    // SEH-safe primitives. Same shape as the helpers in MainWaitProbe.cpp
    // and WaitGraph.cpp — duplicated locally so this translation unit
    // can be compiled without pulling in those headers and so a fault
    // inside Verdict cannot affect the long-form audit.
    // ====================================================================
    namespace {

        bool TryReadQword(std::uintptr_t a_addr, std::uintptr_t& a_out,
                          DWORD& a_seh) noexcept {
            a_seh = 0;
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (a_seh = ::GetExceptionCode(),
                        EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Scan a stack window for a saved return address equal to a_target.
        // Returns true on first match. SEH-bounded: stops on the first
        // unmapped page rather than aborting the surrounding probe.
        bool SawReturnAddr(std::uintptr_t a_rsp,
                           std::uintptr_t a_target,
                           std::size_t    a_windowBytes,
                           DWORD&         a_seh) noexcept {
            a_seh = 0;
            __try {
                for (std::size_t off = 0; off < a_windowBytes;
                     off += sizeof(std::uintptr_t)) {
                    const auto v = *reinterpret_cast<volatile std::uintptr_t*>(
                        a_rsp + off);
                    if (v == a_target) return true;
                }
                return false;
            } __except (a_seh = ::GetExceptionCode(),
                        EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Like SawReturnAddr but returns the first qword whose value lies
        // inside [lo, hi). Useful for "any frame inside this module"
        // probes where we don't know the exact RVA in advance.
        bool SawAddrInRange(std::uintptr_t a_rsp,
                            std::uintptr_t a_lo,
                            std::uintptr_t a_hi,
                            std::size_t    a_windowBytes,
                            std::uintptr_t& a_outHit,
                            DWORD&         a_seh) noexcept {
            a_seh    = 0;
            a_outHit = 0;
            __try {
                for (std::size_t off = 0; off < a_windowBytes;
                     off += sizeof(std::uintptr_t)) {
                    const auto v = *reinterpret_cast<volatile std::uintptr_t*>(
                        a_rsp + off);
                    if (v >= a_lo && v < a_hi) {
                        a_outHit = v;
                        return true;
                    }
                }
                return false;
            } __except (a_seh = ::GetExceptionCode(),
                        EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ----- Thread enumeration ---------------------------------------
        std::vector<DWORD> EnumerateThreads(DWORD a_pid) noexcept {
            std::vector<DWORD> tids;
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return tids;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == a_pid)
                        tids.push_back(te.th32ThreadID);
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return tids;
        }

        // POD result of "suspend, capture context, resume". Stays free of
        // C++ destructors so we can mix it with SEH frames in callers.
        struct ThreadCtx {
            bool           ok  = false;
            std::uintptr_t rip = 0;
            std::uintptr_t rsp = 0;
        };

        ThreadCtx SnapshotThreadCtx(DWORD a_tid) noexcept {
            ThreadCtx out{};
            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!h) return out;
            const DWORD prev = ::SuspendThread(h);
            if (prev == static_cast<DWORD>(-1)) {
                ::CloseHandle(h);
                return out;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            if (::GetThreadContext(h, &ctx)) {
                out.ok  = true;
                out.rip = static_cast<std::uintptr_t>(ctx.Rip);
                out.rsp = static_cast<std::uintptr_t>(ctx.Rsp);
            }
            ::ResumeThread(h);
            ::CloseHandle(h);
            return out;
        }

    }   // anonymous SEH-safe helpers

    // ====================================================================
    // Module table — small cached view of loaded modules. We resolve
    // hdtsmp64.dll once per Observe() call and reuse the result across
    // every per-thread RIP check.
    // ====================================================================
    namespace {

        struct ModuleRange {
            std::uintptr_t base = 0;
            std::uintptr_t size = 0;
            std::string    name;        // base name, lower-cased
            std::string    fileVersion;
        };

        std::string LowerCaseAscii(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Best-effort read of the file's VS_FIXEDFILEINFO. Empty string on
        // failure. Used both here and (in the next pass) by the Modules
        // snapshot to enrich the loaded-modules table.
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

        // Returns the range info for the first loaded module whose base
        // name matches a_baseName (case-insensitive). Default-constructed
        // ModuleRange (size == 0) if not loaded.
        ModuleRange FindModule(std::string_view a_baseName) {
            ModuleRange r;
            DWORD needed = 0;
            ::EnumProcessModulesEx(::GetCurrentProcess(), nullptr, 0, &needed,
                                   LIST_MODULES_ALL);
            if (needed == 0) return r;
            std::vector<HMODULE> handles(needed / sizeof(HMODULE));
            if (!::EnumProcessModulesEx(
                    ::GetCurrentProcess(),
                    handles.data(),
                    static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                    &needed, LIST_MODULES_ALL))
            {
                return r;
            }
            const std::string targetLower{LowerCaseAscii(std::string{a_baseName})};
            for (HMODULE h : handles) {
                char baseName[MAX_PATH] = {};
                if (!::GetModuleBaseNameA(::GetCurrentProcess(), h, baseName,
                                          MAX_PATH))
                    continue;
                if (LowerCaseAscii(baseName) != targetLower) continue;
                MODULEINFO mi{};
                if (!::GetModuleInformation(::GetCurrentProcess(), h, &mi,
                                            sizeof(mi)))
                    continue;
                r.base = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
                r.size = static_cast<std::uintptr_t>(mi.SizeOfImage);
                r.name = baseName;
                wchar_t fullPath[MAX_PATH] = {};
                if (::GetModuleFileNameExW(::GetCurrentProcess(), h,
                                            fullPath, MAX_PATH))
                {
                    r.fileVersion = ReadFileVersion(fullPath);
                }
                return r;
            }
            return r;
        }

    }   // anonymous module helpers

    // ====================================================================
    // Probes — each probe returns its observations into the Observations
    // POD without writing to the report stream.
    // ====================================================================
    namespace {

        struct MainCtxFull {
            bool           ok  = false;
            std::uintptr_t rip = 0;
            std::uintptr_t rbx = 0;
            std::uintptr_t rsp = 0;
        };

        MainCtxFull ReadMainContext(DWORD a_mainTid) noexcept {
            MainCtxFull out{};
            if (a_mainTid == 0) return out;
            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_mainTid);
            if (!h) return out;
            const DWORD prev = ::SuspendThread(h);
            if (prev == static_cast<DWORD>(-1)) {
                ::CloseHandle(h);
                return out;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (::GetThreadContext(h, &ctx)) {
                out.ok  = true;
                out.rip = static_cast<std::uintptr_t>(ctx.Rip);
                out.rbx = static_cast<std::uintptr_t>(ctx.Rbx);
                out.rsp = static_cast<std::uintptr_t>(ctx.Rsp);
            }
            ::ResumeThread(h);
            ::CloseHandle(h);
            return out;
        }

        // POD result of one Singleton-B chain walk.
        struct SingletonBChain {
            bool           ok        = false;
            int            nullStep  = -1;        // 0..4, -1 if intact
            std::uintptr_t globalPtr = 0;
            std::uintptr_t subArray  = 0;
            std::uintptr_t element0  = 0;
            std::uintptr_t vtable    = 0;
            std::uintptr_t handle    = 0;
        };

        SingletonBChain WalkSingletonB(std::uintptr_t a_base) noexcept {
            SingletonBChain c{};
            DWORD seh = 0;
            if (!TryReadQword(a_base + kSingletonBPtrRVA, c.globalPtr, seh)) {
                c.nullStep = 0;
                return c;
            }
            if (c.globalPtr == 0) { c.nullStep = 0; return c; }
            if (!TryReadQword(c.globalPtr + 8, c.subArray, seh)) {
                c.nullStep = 1;
                return c;
            }
            if (c.subArray == 0) { c.nullStep = 1; return c; }
            if (!TryReadQword(c.subArray + 0, c.element0, seh)) {
                c.nullStep = 2;
                return c;
            }
            if (c.element0 == 0) { c.nullStep = 2; return c; }
            if (!TryReadQword(c.element0 + 0, c.vtable, seh)) {
                c.nullStep = 3;
                return c;
            }
            if (c.vtable == 0) { c.nullStep = 3; return c; }
            if (!TryReadQword(c.vtable + 8, c.handle, seh)) {
                c.nullStep = 4;
                return c;
            }
            c.ok = true;
            return c;
        }

        // True iff the same chain values come back twice ~50 ms apart.
        // Used as a writer-still-live probe — a chain that's mutating
        // during the sample window is a live writer; one that doesn't
        // change is in steady-state deadlock.
        bool ChainStable(std::uintptr_t a_base, const SingletonBChain& a_first) {
            ::Sleep(50);
            const auto second = WalkSingletonB(a_base);
            return second.globalPtr == a_first.globalPtr &&
                   second.subArray  == a_first.subArray  &&
                   second.element0  == a_first.element0  &&
                   second.vtable    == a_first.vtable    &&
                   second.handle    == a_first.handle    &&
                   second.nullStep  == a_first.nullStep;
        }

        // ----- HDT-SMP worker pool counter ------------------------------
        // Walk every thread; record any whose top frame RIP is inside
        // the hdtsmp64.dll module's range. Reports the modal RVA so the
        // verdict can say "all parked at hdtsmp64.dll+0xXXXXXX" when the
        // workers truly are uniform.
        struct WorkerPoolStats {
            int            count   = 0;
            std::uintptr_t modalRva = 0;  // 0 when threads sit at different RVAs
        };

        WorkerPoolStats CountWorkersInHdtsmp(
            const ModuleRange& a_mod,
            DWORD              a_selfTid,
            DWORD              a_mainTid) noexcept
        {
            WorkerPoolStats s{};
            if (a_mod.size == 0) return s;
            const auto lo = a_mod.base;
            const auto hi = a_mod.base + a_mod.size;

            const DWORD pid  = ::GetCurrentProcessId();
            const auto  tids = EnumerateThreads(pid);

            // We only care whether all matched threads share a single
            // RVA. Two-pass O(N) algorithm using a small inline tally:
            // we remember the first seen RVA, then check that every
            // subsequent match equals it; any mismatch sets modal=0.
            std::uintptr_t firstRva = 0;
            bool           uniform  = true;
            for (DWORD tid : tids) {
                if (tid == a_selfTid || tid == a_mainTid) continue;
                const auto t = SnapshotThreadCtx(tid);
                if (!t.ok || t.rip < lo || t.rip >= hi) continue;
                const auto rva = t.rip - a_mod.base;
                if (s.count == 0) {
                    firstRva = rva;
                } else if (rva != firstRva) {
                    uniform = false;
                }
                ++s.count;
            }
            s.modalRva = (s.count > 0 && uniform) ? firstRva : 0;
            return s;
        }

        // ----- BSSpinLock cycle search ----------------------------------
        // Returns (any-spinner, owner-equals-main). We do not need to
        // surface the lock pointer at the verdict layer — the long-form
        // MainWaitProbe does that. We only need the yes/no.
        struct SpinlockState {
            bool spinnerSeen   = false;
            bool ownerIsMain   = false;
        };

        SpinlockState ScanSpinlocks(std::uintptr_t a_base,
                                    DWORD          a_selfTid,
                                    DWORD          a_mainTid) noexcept
        {
            SpinlockState out{};
            const std::uintptr_t spinRetAddr = a_base + kSpinRetRVA;

            const DWORD pid  = ::GetCurrentProcessId();
            const auto  tids = EnumerateThreads(pid);

            for (DWORD tid : tids) {
                if (tid == a_selfTid) continue;
                const HANDLE h = ::OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                        THREAD_QUERY_LIMITED_INFORMATION,
                    FALSE, tid);
                if (!h) continue;
                const DWORD prev = ::SuspendThread(h);
                if (prev == static_cast<DWORD>(-1)) {
                    ::CloseHandle(h);
                    continue;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
                const bool gotCtx = ::GetThreadContext(h, &ctx) != FALSE;
                ::ResumeThread(h);
                ::CloseHandle(h);
                if (!gotCtx) continue;

                // Is this thread spinning at the BSSpinLock retry RVA?
                DWORD seh = 0;
                if (!SawReturnAddr(static_cast<std::uintptr_t>(ctx.Rsp),
                                   spinRetAddr, kSpinScanWindow, seh))
                {
                    continue;
                }
                out.spinnerSeen = true;

                // Scan registers for any qword whose [+0] dword equals
                // a_mainTid (the BSSpinLock layout: { owner_tid; state; }).
                const std::uintptr_t cand[] = {
                    static_cast<std::uintptr_t>(ctx.Rax),
                    static_cast<std::uintptr_t>(ctx.Rbx),
                    static_cast<std::uintptr_t>(ctx.Rcx),
                    static_cast<std::uintptr_t>(ctx.Rdx),
                    static_cast<std::uintptr_t>(ctx.Rsi),
                    static_cast<std::uintptr_t>(ctx.Rdi),
                    static_cast<std::uintptr_t>(ctx.R8),
                    static_cast<std::uintptr_t>(ctx.R9),
                    static_cast<std::uintptr_t>(ctx.R10),
                    static_cast<std::uintptr_t>(ctx.R11),
                    static_cast<std::uintptr_t>(ctx.R12),
                    static_cast<std::uintptr_t>(ctx.R13),
                    static_cast<std::uintptr_t>(ctx.R14),
                    static_cast<std::uintptr_t>(ctx.R15),
                };
                for (auto v : cand) {
                    if (v < 0x10000u || v > 0x00007FFFFFFFFFFFu) continue;
                    if ((v & 0x3) != 0) continue;
                    std::uintptr_t pair = 0;
                    DWORD seh2 = 0;
                    if (!TryReadQword(v, pair, seh2)) continue;
                    const auto owner = static_cast<std::uint32_t>(pair & 0xffffffff);
                    if (owner == a_mainTid && owner != 0) {
                        out.ownerIsMain = true;
                        return out;
                    }
                }
            }
            return out;
        }

        // ----- Top-level Observe orchestrator ---------------------------
        Observations Observe() noexcept {
            Observations obs;

            const HMODULE skyrim = ::GetModuleHandleW(L"SkyrimSE.exe");
            if (!skyrim) return obs;
            const auto base = reinterpret_cast<std::uintptr_t>(skyrim);

            const auto mainTid = static_cast<DWORD>(Heartbeat::MainTid());
            const auto selfTid = ::GetCurrentThreadId();
            if (mainTid == 0) return obs;

            // 1. Main thread RIP / stack — feed wait-site detection.
            const auto m = ReadMainContext(mainTid);
            if (!m.ok) return obs;

            const bool ripInLockFnA =
                m.rip >= base + kLockFnLoRVA && m.rip < base + kLockFnHiRVA;
            const bool ripInWrapperB =
                m.rip >= base + kWaitWrapperLoRVA &&
                m.rip <  base + kWaitWrapperHiRVA;

            DWORD seh = 0;
            const bool retAInStack = SawReturnAddr(
                m.rsp, base + kLockReturnRVA, kSiteScanWindow, seh);
            const bool retBInStack = SawReturnAddr(
                m.rsp, base + kMainUpdateRetBRVA, kSiteScanWindow, seh);

            obs.inSiteA = ripInLockFnA  || retAInStack;
            obs.inSiteB = ripInWrapperB || retBInStack;

            // 2. Singleton-B chain (when we're at Site B).
            if (obs.inSiteB) {
                const auto chain = WalkSingletonB(base);
                obs.siteBChainWalked = true;
                obs.siteBChainOk     = chain.ok;
                obs.siteBNullStep    = chain.nullStep;
                obs.siteBChainStable = ChainStable(base, chain);
            }

            // 3. HDT-SMP fingerprint — find the loaded module, then ask
            //    whether any saved return address on main's stack points
            //    inside it, and count threads parked inside it.
            const auto smp = FindModule("hdtsmp64.dll");
            obs.hdtsmpLoaded = (smp.size > 0);
            if (obs.hdtsmpLoaded) {
                obs.hdtsmpVersion = smp.fileVersion;

                std::uintptr_t hit = 0;
                DWORD          rangeSeh = 0;
                if (SawAddrInRange(m.rsp, smp.base, smp.base + smp.size,
                                   kHdtsmpScanWindow, hit, rangeSeh))
                {
                    obs.hdtsmpOnMainStack = true;
                    obs.hdtsmpFrameRva    = hit - smp.base;
                }

                const auto wp = CountWorkersInHdtsmp(smp, selfTid, mainTid);
                obs.hdtsmpWorkerPoolIdle = wp.count;
                obs.hdtsmpWorkerWaitRva  = wp.modalRva;
            }

            // 4. BSSpinLock cycle (Site-A AB-BA fingerprint).
            const auto spin = ScanSpinlocks(base, selfTid, mainTid);
            obs.spinlockSpinnerSeen = spin.spinnerSeen;
            obs.spinlockOwnedByMain = spin.ownerIsMain;

            return obs;
        }

    }   // anonymous probe namespace

    // ====================================================================
    // Pure classifier — testable in isolation. The ordering matters:
    // BSSpinLock AB-BA wins when both Site A and a main-owned spinner
    // are present (that's the WSF class regardless of how Singleton-A
    // looks); HDT-SMP / Site-B requires both Site B AND the HDT-SMP
    // fingerprint on the stack; the bare Site-B class is a fallback.
    // ====================================================================
    Classified Classify(const Observations& a_obs) {
        // 1. BSSpinLock AB-BA — high confidence when owner is main.
        if (a_obs.inSiteA && a_obs.spinlockOwnedByMain) {
            return {
                Class::SpinlockAbBa,
                "BSSpinLock AB-BA (WorkerSpinLockFix domain)",
                "high",
                "docs/case-study/06-root-cause.md",
            };
        }

        // 2. HDT-SMP / Site-B — requires both the Site-B wait AND a
        //    frame inside hdtsmp64.dll above main's RSP. Confidence
        //    climbs with the Singleton-B chain being torn down and a
        //    visible worker pool.
        if (a_obs.inSiteB && a_obs.hdtsmpOnMainStack) {
            std::string conf = "medium";
            if (a_obs.siteBChainWalked && !a_obs.siteBChainOk &&
                a_obs.hdtsmpWorkerPoolIdle >= 1)
            {
                conf = "high";
            }
            return {
                Class::HdtsmpSiteB,
                "HDT-SMP / Site-B Papyrus event-source wait",
                conf,
                "docs/case-study/27-hdtsmp-deadlock-report.md",
            };
        }

        // 3. Site-B without the HDT-SMP fingerprint — same bug class but
        //    proximate caller is something else.
        if (a_obs.inSiteB) {
            return {
                Class::SiteBNoHdtsmp,
                "Site-B Papyrus event-source wait (no HDT-SMP fingerprint)",
                a_obs.siteBChainWalked && !a_obs.siteBChainOk ? "medium" : "low",
                "docs/case-study/27-hdtsmp-deadlock-report.md",
            };
        }

        // 4. Site-A without a spinlock cycle — worker-ack class.
        if (a_obs.inSiteA) {
            return {
                Class::SiteAWorkerAck,
                "Site-A worker-ack wait (no BSSpinLock cycle observed)",
                a_obs.spinlockSpinnerSeen ? "medium" : "low",
                "docs/case-study/06-root-cause.md",
            };
        }

        return {
            Class::Unrecognised,
            "Unrecognised — main is in a kernel wait we don't have a fingerprint for",
            "low",
            "docs/spec.md",
        };
    }

    // ====================================================================
    // Formatter
    // ====================================================================
    namespace {
        const char* SiteString(const Observations& o) {
            if (o.inSiteA && o.inSiteB) return "A+B (?)";
            if (o.inSiteA) return "A (Singleton-A id 34554 lock primitive)";
            if (o.inSiteB) return "B (Singleton-B +0xc38130 event-source wrapper)";
            return "unrecognised";
        }

        const char* ChainStateString(const Observations& o) {
            if (!o.siteBChainWalked) return "not-walked";
            if (o.siteBChainOk)      return "intact";
            switch (o.siteBNullStep) {
            case 0: return "zeroed at step 0 (global ptr)";
            case 1: return "zeroed at step 1 (sub-array @ +8)";
            case 2: return "zeroed at step 2 (element[0])";
            case 3: return "zeroed at step 3 (vtable)";
            case 4: return "zeroed at step 4 (handle)";
            default: return "broken (no step recorded)";
            }
        }
    }

    void WriteFromObservations(std::ostream& a_os, const Observations& a_obs) {
        const auto v = Classify(a_obs);

        a_os << "Freeze class:        " << v.label              << "\n";
        a_os << "Confidence:          " << v.confidence         << "\n";
        a_os << "Site:                " << SiteString(a_obs)    << "\n";

        if (a_obs.inSiteB) {
            a_os << "Singleton-B chain:   " << ChainStateString(a_obs);
            if (a_obs.siteBChainWalked) {
                a_os << " (writer: "
                     << (a_obs.siteBChainStable ? "settled" : "still mutating")
                     << ")";
            }
            a_os << "\n";
        }

        if (a_obs.hdtsmpLoaded) {
            a_os << "HDT-SMP loaded:      yes";
            if (!a_obs.hdtsmpVersion.empty()) {
                a_os << " (FileVersion " << a_obs.hdtsmpVersion << ")";
            }
            a_os << "\n";

            if (a_obs.hdtsmpOnMainStack) {
                a_os << std::format(
                    "HDT-SMP on stack:    yes (frame at hdtsmp64.dll+0x{:x})\n",
                    a_obs.hdtsmpFrameRva);
            } else {
                a_os << "HDT-SMP on stack:    no\n";
            }

            if (a_obs.hdtsmpWorkerPoolIdle > 0) {
                if (a_obs.hdtsmpWorkerWaitRva != 0) {
                    a_os << std::format(
                        "HDT-SMP worker pool: {} idle (all parked on "
                        "hdtsmp64.dll+0x{:x})\n",
                        a_obs.hdtsmpWorkerPoolIdle,
                        a_obs.hdtsmpWorkerWaitRva);
                } else {
                    a_os << std::format(
                        "HDT-SMP worker pool: {} thread(s) inside hdtsmp64.dll "
                        "(varied RVAs)\n",
                        a_obs.hdtsmpWorkerPoolIdle);
                }
            } else {
                a_os << "HDT-SMP worker pool: 0 idle\n";
            }
        } else {
            a_os << "HDT-SMP loaded:      no\n";
        }

        if (a_obs.spinlockSpinnerSeen) {
            a_os << "BSSpinLock spinner:  yes"
                 << (a_obs.spinlockOwnedByMain ? " (owner == MAIN — AB-BA cycle)" : "")
                 << "\n";
        }

        a_os << "Suggested triage:    " << v.triageDoc << "\n";
    }

    void Write(std::ostream& a_os) {
        const auto obs = Observe();
        WriteFromObservations(a_os, obs);
    }

}
