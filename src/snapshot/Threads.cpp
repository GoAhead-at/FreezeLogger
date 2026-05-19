#include "PCH.h"
#include "snapshot/Threads.h"

#include "AddrLib.h"
#include "Config.h"
#include "Heartbeat.h"
#include "Symbols.h"

#include <TlHelp32.h>

#include <algorithm>

namespace FreezeLogger::Snapshot::Threads {

    namespace {

        // RAII guard around SuspendThread / ResumeThread. CRITICAL: any code
        // path inside WalkOne that fails to resume the suspended thread will
        // leave the game permanently frozen. Using a guard means even an
        // exception bubbling up from DbgHelp (or from std::format / OOM /
        // anything else) is unwound through the destructor, which calls
        // ResumeThread.
        class SuspendGuard {
        public:
            SuspendGuard() = default;
            explicit SuspendGuard(HANDLE a_thread) : _thread(a_thread) {
                if (_thread) {
                    const DWORD prev = ::SuspendThread(_thread);
                    if (prev == static_cast<DWORD>(-1)) {
                        _lastError = ::GetLastError();
                        _thread    = nullptr;  // we don't own a suspension
                    } else {
                        _suspended = true;
                    }
                }
            }
            SuspendGuard(const SuspendGuard&)            = delete;
            SuspendGuard& operator=(const SuspendGuard&) = delete;
            SuspendGuard(SuspendGuard&& o) noexcept
                : _thread(o._thread), _suspended(o._suspended), _lastError(o._lastError) {
                o._thread    = nullptr;
                o._suspended = false;
                o._lastError = 0;
            }
            ~SuspendGuard() {
                if (_thread && _suspended) {
                    ::ResumeThread(_thread);
                }
            }
            bool   ok()        const noexcept { return _suspended; }
            DWORD  lastError() const noexcept { return _lastError; }
        private:
            HANDLE _thread     = nullptr;
            bool   _suspended  = false;
            DWORD  _lastError  = 0;
        };

        std::vector<DWORD> EnumerateThreads(DWORD a_pid) {
            std::vector<DWORD> tids;
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return tids;

            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == a_pid) {
                        tids.push_back(te.th32ThreadID);
                    }
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return tids;
        }

        std::string ThreadDescription(HANDLE a_thread) {
            using GetThreadDescriptionFn = HRESULT (WINAPI*)(HANDLE, PWSTR*);
            static auto fn = []() -> GetThreadDescriptionFn {
                if (auto k32 = ::GetModuleHandleW(L"kernel32.dll")) {
                    return reinterpret_cast<GetThreadDescriptionFn>(
                        ::GetProcAddress(k32, "GetThreadDescription"));
                }
                return nullptr;
            }();
            if (!fn) return {};

            PWSTR desc = nullptr;
            if (FAILED(fn(a_thread, &desc)) || !desc) return {};
            std::wstring w(desc);
            ::LocalFree(desc);
            if (w.empty()) return {};

            const int n = ::WideCharToMultiByte(
                CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
            std::string out(n, '\0');
            ::WideCharToMultiByte(
                CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
            return out;
        }

        const char* RoleLabel(DWORD a_tid, DWORD a_mainTid, DWORD a_renderTid) {
            if (a_mainTid   != 0 && a_tid == a_mainTid)   return " [main game thread]";
            if (a_renderTid != 0 && a_tid == a_renderTid) return " [render thread]";
            return "";
        }

        // ---------- ntdll!NtQueryEvent (read-only event probe) ---------------
        // Same shape as MainWaitProbe.cpp; duplicated locally to keep this
        // module independent and avoid leaking helpers across snapshot
        // boundaries. The probe is read-only — no signal is consumed.
        struct EVENT_BASIC_INFORMATION_ {
            LONG EventType;   // 0=NotificationEvent (manual), 1=SynchronizationEvent (auto)
            LONG EventState;  // 0=non-signaled, 1=signaled
        };
        using NtQueryEventFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        NtQueryEventFn LoadNtQueryEvent() noexcept {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (!ntdll) return nullptr;
            return reinterpret_cast<NtQueryEventFn>(
                ::GetProcAddress(ntdll, "NtQueryEvent"));
        }

        // The substrings we use to recognise blocked-wait positions in the
        // top frame's symbol. Order doesn't matter; presence is enough.
        bool TopSymLooksLikeWaitOnHandle(const std::string& a_sym) {
            // Single-handle wait paths only. Multi-object waits use an
            // array of handles, which we cannot trivially probe from a
            // suspended thread's register state.
            return  a_sym.find("WaitForSingleObject")   != std::string::npos
                ||  a_sym.find("ZwWaitForSingleObject") != std::string::npos
                ||  a_sym.find("NtWaitForSingleObject") != std::string::npos;
        }

        const char* EventTypeName(LONG a_t) {
            switch (a_t) {
            case 0: return "NotificationEvent (manual)";
            case 1: return "SynchronizationEvent (auto)";
            default: return "?";
            }
        }

        // Format a frame: hex address, symbolic name, and an Address Library
        // ID annotation when available. Suppressed when a_addrlib already
        // matched the symbol (e.g. SymFromAddr resolved by name) to keep
        // lines compact.
        std::string FormatFrame(std::uintptr_t a_pc, std::string a_sym) {
            const auto annot = AddrLib::FormatAnnotation(a_pc);
            if (annot.empty()) {
                return std::format("0x{:016x}  {}", a_pc, a_sym);
            }
            return std::format("0x{:016x}  {}  {}", a_pc, a_sym, annot);
        }

        // Auto-correlate a register value against the small set of things we
        // already know are interesting in the deadlock investigation.
        // Returns "" when nothing matches; otherwise a short suffix like
        // "(=Singleton-A *)".
        std::string CorrelateRegister(std::uintptr_t a_value, std::uintptr_t a_skyrimBase) {
            if (a_skyrimBase == 0 || a_value == 0) return {};

            // Singleton-A pointer slot (id34554 lock primitive)
            const auto sA = a_skyrimBase + 0x2f26668;
            // Singleton-A struct address (loaded by main loop)
            const auto sAStruct = a_skyrimBase + 0x2f26680;
            // Singleton-B pointer slot (Site B wait, +0xc38130 wrapper)
            const auto sB = a_skyrimBase + 0x2f26a70;

            if (a_value == sA)        return " (=&Singleton-A.ptrSlot)";
            if (a_value == sAStruct)  return " (=Singleton-A struct)";
            if (a_value == sB)        return " (=&Singleton-B.ptrSlot)";
            return {};
        }

        // Walk one thread's stack, isolated. Any exception raised inside
        // the body is caught here, so the SuspendGuard always runs and the
        // surrounding loop can continue with the next thread.
        void WalkOneInner(std::ostream& a_os,
                          HANDLE        a_thread,
                          DWORD         a_tid,
                          DWORD         a_mainTid,
                          DWORD         a_renderTid)
        {
            const auto desc = ThreadDescription(a_thread);
            a_os << "  TID " << a_tid << RoleLabel(a_tid, a_mainTid, a_renderTid);
            if (!desc.empty()) a_os << "  \"" << desc << "\"";
            a_os << "\n";

            SuspendGuard suspend{a_thread};
            if (!suspend.ok()) {
                a_os << "    <SuspendThread failed: " << suspend.lastError() << ">\n\n";
                return;
            }

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!::GetThreadContext(a_thread, &ctx)) {
                a_os << "    <GetThreadContext failed: " << ::GetLastError() << ">\n\n";
                return;
            }

            STACKFRAME64 frame{};
            frame.AddrPC.Mode    = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
#if defined(_M_X64)
            frame.AddrPC.Offset    = ctx.Rip;
            frame.AddrFrame.Offset = ctx.Rbp;
            frame.AddrStack.Offset = ctx.Rsp;
            const DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
#else
#error FreezeLogger only supports x64.
#endif

            const auto kMaxFrames = static_cast<int>(
                std::max<std::uint32_t>(1u, Config::Get().snapshot.max_frames_per_stack));
            int frameNo = 0;

            // Capture the top frame's symbol so we can decide whether to
            // probe an event handle for this thread after the walk ends.
            std::string topSym;

            {
                // Hold the symbols mutex for the whole walk: DbgHelp is not
                // thread-safe. ResolveLocked() below skips re-acquiring it,
                // which is what makes the loop deadlock-free.
                Symbols::Lock symbolLock;

                while (frameNo < kMaxFrames &&
                       ::StackWalk64(
                           machineType,
                           ::GetCurrentProcess(),
                           a_thread,
                           &frame,
                           &ctx,
                           nullptr,
                           ::SymFunctionTableAccess64,
                           ::SymGetModuleBase64,
                           nullptr))
                {
                    if (frame.AddrPC.Offset == 0) break;
                    const auto pc  = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
                    const auto sym = Symbols::ResolveLocked(pc);
                    if (frameNo == 0) topSym = sym;
                    a_os << std::format("    #{:02} {}\n",
                                        frameNo, FormatFrame(pc, sym));
                    ++frameNo;
                }
            }   // release Symbols::Lock before any further work

            // ----- Per-thread non-volatile register dump --------------------
            // Walking the registers AFTER the StackWalk call avoids any
            // confusion with the local CONTEXT being mutated. ctx still
            // reflects the OS-captured initial register state; StackWalk64
            // mutates a *copy* internally, but on x64 with no nullptr
            // ContextRecord param, ctx may be modified per the docs. So
            // we re-fetch the OS context for the print line.
            CONTEXT regCtx{};
            regCtx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            const bool regOk = ::GetThreadContext(a_thread, &regCtx) != 0;

            const auto skyrimMod = ::GetModuleHandleW(L"SkyrimSE.exe");
            const std::uintptr_t base = skyrimMod
                ? reinterpret_cast<std::uintptr_t>(skyrimMod)
                : 0;

            if (regOk) {
                a_os << std::format(
                    "    nv-regs: RBX={:#018x}{} RBP={:#018x}{} RSI={:#018x}{} RDI={:#018x}{}\n",
                    static_cast<std::uintptr_t>(regCtx.Rbx),
                    CorrelateRegister(regCtx.Rbx, base),
                    static_cast<std::uintptr_t>(regCtx.Rbp),
                    CorrelateRegister(regCtx.Rbp, base),
                    static_cast<std::uintptr_t>(regCtx.Rsi),
                    CorrelateRegister(regCtx.Rsi, base),
                    static_cast<std::uintptr_t>(regCtx.Rdi),
                    CorrelateRegister(regCtx.Rdi, base));
                a_os << std::format(
                    "             R12={:#018x}{} R13={:#018x}{} R14={:#018x}{} R15={:#018x}{}\n",
                    static_cast<std::uintptr_t>(regCtx.R12),
                    CorrelateRegister(regCtx.R12, base),
                    static_cast<std::uintptr_t>(regCtx.R13),
                    CorrelateRegister(regCtx.R13, base),
                    static_cast<std::uintptr_t>(regCtx.R14),
                    CorrelateRegister(regCtx.R14, base),
                    static_cast<std::uintptr_t>(regCtx.R15),
                    CorrelateRegister(regCtx.R15, base));
            }

            // ----- Per-thread "what are you waiting on?" --------------------
            // For threads parked in WaitForSingleObject{,Ex} the handle
            // sits in RBX (preserved by the user-mode wait wrappers).
            // NtQueryEvent gives us {type, signaled} non-destructively.
            // Multi-object waits use an array of handles which we cannot
            // probe from a single register, so we silently skip them.
            if (regOk && TopSymLooksLikeWaitOnHandle(topSym)) {
                static const auto pNtQueryEvent = LoadNtQueryEvent();
                const auto h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(regCtx.Rbx));
                if (pNtQueryEvent && h && h != reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(-1))) {
                    EVENT_BASIC_INFORMATION_ info{};
                    ULONG ret = 0;
                    const LONG status = pNtQueryEvent(h, /*EventBasicInformation=*/0,
                                                      &info, sizeof(info), &ret);
                    if (status >= 0) {
                        a_os << std::format(
                            "    waiting on: HANDLE={:#x} [{}, {}]\n",
                            reinterpret_cast<std::uintptr_t>(h),
                            EventTypeName(info.EventType),
                            info.EventState ? "SIGNALED" : "NOT signaled");
                    } else {
                        a_os << std::format(
                            "    waiting on: HANDLE={:#x} <NtQueryEvent NTSTATUS=0x{:08x}>\n",
                            reinterpret_cast<std::uintptr_t>(h),
                            static_cast<std::uint32_t>(status));
                    }
                }
            }
        }

        void WalkOne(std::ostream& a_os,
                     DWORD         a_tid,
                     DWORD         a_selfTid,
                     DWORD         a_mainTid,
                     DWORD         a_renderTid)
        {
            if (a_tid == a_selfTid) {
                a_os << "  TID " << a_tid << "  [watchdog thread, skipped]\n\n";
                return;
            }

            HANDLE hThread = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!hThread) {
                a_os << "  TID " << a_tid << "  <OpenThread failed: "
                     << ::GetLastError() << ">\n\n";
                return;
            }

            // Belt-and-braces: catch ANY exception so the SuspendGuard inside
            // WalkOneInner is guaranteed to run and ResumeThread fires.
            try {
                WalkOneInner(a_os, hThread, a_tid, a_mainTid, a_renderTid);
            } catch (const std::exception& e) {
                a_os << "    <walk aborted, C++ exception: " << e.what() << ">\n";
            } catch (...) {
                a_os << "    <walk aborted, unknown C++ exception>\n";
            }

            ::CloseHandle(hThread);
            a_os << "\n";
        }

        // Reorder the TID list so the main and render threads are at the
        // front. Snapshots are bounded by Snapshot::max_threads, and the
        // stalled thread's stack is the most useful diagnostic, so we make
        // sure it's never dropped from the cap.
        void PrioritizeThreads(std::vector<DWORD>& a_tids,
                               DWORD               a_mainTid,
                               DWORD               a_renderTid)
        {
            auto move_front = [&a_tids](DWORD a_tid) {
                if (a_tid == 0) return;
                auto it = std::find(a_tids.begin(), a_tids.end(), a_tid);
                if (it == a_tids.end() || it == a_tids.begin()) return;
                std::rotate(a_tids.begin(), it, it + 1);
            };
            // Insert render first then main, so main ends up at index 0.
            move_front(a_renderTid);
            move_front(a_mainTid);
        }

    }

    void Write(std::ostream& a_os) {
        const auto pid       = ::GetCurrentProcessId();
        const auto selfTid   = ::GetCurrentThreadId();
        const auto mainTid   = static_cast<DWORD>(Heartbeat::MainTid());
        const auto renderTid = static_cast<DWORD>(Heartbeat::RenderTid());
        auto       tids      = EnumerateThreads(pid);
        const auto totalTids = tids.size();

        const auto cap = Config::Get().snapshot.max_threads;
        PrioritizeThreads(tids, mainTid, renderTid);
        if (cap > 0 && tids.size() > cap) {
            tids.resize(cap);
        }

        a_os << totalTids << " threads in process " << pid
             << " (self=" << selfTid;
        if (mainTid)   a_os << ", main="   << mainTid;
        if (renderTid) a_os << ", render=" << renderTid;
        a_os << "; walking " << tids.size() << "):\n\n";

        for (DWORD tid : tids) {
            WalkOne(a_os, tid, selfTid, mainTid, renderTid);
        }
    }

}
