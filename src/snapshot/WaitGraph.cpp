#include "PCH.h"
#include "snapshot/WaitGraph.h"

#include "Heartbeat.h"
#include "Symbols.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace FreezeLogger::Snapshot::WaitGraph {

    namespace {

        // ---- ntdll!NtQueryEvent ---------------------------------------------
        struct EVENT_BASIC_INFORMATION_ {
            LONG EventType;
            LONG EventState;
        };
        using NtQueryEventFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        NtQueryEventFn LoadNtQueryEvent() noexcept {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (!ntdll) return nullptr;
            return reinterpret_cast<NtQueryEventFn>(
                ::GetProcAddress(ntdll, "NtQueryEvent"));
        }

        // RAII suspend guard. Mirrors Threads.cpp; duplicated to keep this
        // module independent.
        class SuspendGuard {
        public:
            SuspendGuard() = default;
            explicit SuspendGuard(HANDLE a_thread) : _thread(a_thread) {
                if (_thread) {
                    if (::SuspendThread(_thread) != static_cast<DWORD>(-1)) {
                        _suspended = true;
                    } else {
                        _thread = nullptr;
                    }
                }
            }
            SuspendGuard(const SuspendGuard&)            = delete;
            SuspendGuard& operator=(const SuspendGuard&) = delete;
            ~SuspendGuard() {
                if (_thread && _suspended) ::ResumeThread(_thread);
            }
            bool ok() const noexcept { return _suspended; }
        private:
            HANDLE _thread    = nullptr;
            bool   _suspended = false;
        };

        struct WaiterRow {
            DWORD          tid       = 0;
            std::uintptr_t handle    = 0;
            std::string    topSym;
            // All non-volatile registers; later used to find threads that
            // *touch* a given handle even if they aren't waiting on it.
            std::uintptr_t rbx = 0;
            std::uintptr_t rbp = 0;
            std::uintptr_t rsi = 0;
            std::uintptr_t rdi = 0;
            std::uintptr_t r12 = 0;
            std::uintptr_t r13 = 0;
            std::uintptr_t r14 = 0;
            std::uintptr_t r15 = 0;
            // Filled if NtQueryEvent succeeded:
            bool           queried   = false;
            LONG           eventType = 0;
            LONG           signaled  = 0;
        };

        bool TopSymLooksLikeWaitOnHandle(const std::string& a_sym) {
            return  a_sym.find("WaitForSingleObject")   != std::string::npos
                ||  a_sym.find("ZwWaitForSingleObject") != std::string::npos
                ||  a_sym.find("NtWaitForSingleObject") != std::string::npos;
        }

        std::vector<DWORD> EnumerateThreads(DWORD a_pid) {
            std::vector<DWORD> tids;
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return tids;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == a_pid) tids.push_back(te.th32ThreadID);
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return tids;
        }

        // Returns the symbol of the topmost frame for a snapshotted thread.
        // ctx is taken by value because StackWalk64 may modify it.
        std::string ResolveTopFrame(HANDLE a_thread, CONTEXT a_ctx) {
            STACKFRAME64 f{};
            f.AddrPC.Mode    = AddrModeFlat;
            f.AddrFrame.Mode = AddrModeFlat;
            f.AddrStack.Mode = AddrModeFlat;
            f.AddrPC.Offset    = a_ctx.Rip;
            f.AddrFrame.Offset = a_ctx.Rbp;
            f.AddrStack.Offset = a_ctx.Rsp;

            Symbols::Lock lk;
            if (!::StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                               ::GetCurrentProcess(), a_thread,
                               &f, &a_ctx, nullptr,
                               ::SymFunctionTableAccess64,
                               ::SymGetModuleBase64, nullptr))
            {
                return {};
            }
            if (f.AddrPC.Offset == 0) return {};
            return Symbols::ResolveLocked(static_cast<std::uintptr_t>(f.AddrPC.Offset));
        }

        std::optional<WaiterRow> ProbeOne(DWORD a_tid, DWORD a_selfTid) {
            if (a_tid == a_selfTid) return std::nullopt;

            HANDLE hThread = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!hThread) return std::nullopt;

            // Always close the handle on exit.
            struct H { HANDLE h; ~H() { if (h) ::CloseHandle(h); } } guard{ hThread };

            SuspendGuard suspend{ hThread };
            if (!suspend.ok()) return std::nullopt;

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!::GetThreadContext(hThread, &ctx)) return std::nullopt;

            const auto sym = ResolveTopFrame(hThread, ctx);
            if (!TopSymLooksLikeWaitOnHandle(sym)) return std::nullopt;

            WaiterRow r;
            r.tid    = a_tid;
            r.topSym = sym;
            r.handle = static_cast<std::uintptr_t>(ctx.Rbx);
            r.rbx    = ctx.Rbx;
            r.rbp    = ctx.Rbp;
            r.rsi    = ctx.Rsi;
            r.rdi    = ctx.Rdi;
            r.r12    = ctx.R12;
            r.r13    = ctx.R13;
            r.r14    = ctx.R14;
            r.r15    = ctx.R15;

            static const auto pNtQueryEvent = LoadNtQueryEvent();
            if (pNtQueryEvent && r.handle != 0 && r.handle != static_cast<std::uintptr_t>(-1)) {
                EVENT_BASIC_INFORMATION_ info{};
                ULONG ret = 0;
                const LONG s = pNtQueryEvent(reinterpret_cast<HANDLE>(r.handle),
                                             0, &info, sizeof(info), &ret);
                if (s >= 0) {
                    r.queried   = true;
                    r.eventType = info.EventType;
                    r.signaled  = info.EventState;
                }
            }

            return r;
        }

        const char* EventTypeName(LONG t) {
            switch (t) {
            case 0: return "NotificationEvent";
            case 1: return "SynchronizationEvent";
            default: return "?";
            }
        }

    }

    void Write(std::ostream& a_os) {
        const auto pid     = ::GetCurrentProcessId();
        const auto selfTid = ::GetCurrentThreadId();
        const auto mainTid = static_cast<DWORD>(Heartbeat::MainTid());

        const auto tids = EnumerateThreads(pid);
        std::vector<WaiterRow> waiters;
        waiters.reserve(tids.size());

        for (DWORD tid : tids) {
            try {
                auto r = ProbeOne(tid, selfTid);
                if (r) waiters.push_back(std::move(*r));
            } catch (...) {
                // never let one bad thread abort the whole probe
            }
        }

        a_os << waiters.size() << " threads parked in WaitForSingleObject*"
             << " (process " << pid << ").\n\n";

        if (waiters.empty()) return;

        // ---- Group by handle ------------------------------------------------
        std::map<std::uintptr_t, std::vector<const WaiterRow*>> byHandle;
        for (const auto& w : waiters) byHandle[w.handle].push_back(&w);

        a_os << "Per-handle wait table:\n";
        for (const auto& [h, group] : byHandle) {
            // Pick representative state from the first queried row.
            const WaiterRow* rep = nullptr;
            for (const auto* w : group) { if (w->queried) { rep = w; break; } }

            a_os << std::format("  HANDLE 0x{:x}", h);
            if (rep) {
                a_os << std::format(" [{}, {}]",
                                    EventTypeName(rep->eventType),
                                    rep->signaled ? "SIGNALED" : "NOT signaled");
            } else {
                a_os << " [state: <NtQueryEvent failed for all waiters>]";
            }
            a_os << std::format(" - {} waiter(s):\n", group.size());

            for (const auto* w : group) {
                const bool isMain = (mainTid != 0 && w->tid == mainTid);
                a_os << std::format("    TID {}{}\n",
                                    w->tid, isMain ? " [MAIN]" : "");
            }

            // Cross-thread: which OTHER threads (not waiters of this handle)
            // have this handle anywhere in their non-volatile registers?
            // That points at the producer / signaller candidate.
            std::unordered_set<DWORD> waiterSet;
            for (const auto* w : group) waiterSet.insert(w->tid);

            std::vector<DWORD> touchers;
            for (const auto& other : waiters) {
                if (waiterSet.count(other.tid)) continue;
                if (other.rbx == h || other.rbp == h || other.rsi == h ||
                    other.rdi == h || other.r12 == h || other.r13 == h ||
                    other.r14 == h || other.r15 == h)
                {
                    touchers.push_back(other.tid);
                }
            }
            if (!touchers.empty()) {
                a_os << "    other threads that reference this handle in regs:\n";
                for (auto tid : touchers) a_os << "      TID " << tid << "\n";
            }
        }

        a_os << "\nSummary:\n";
        // A canonical deadlock signature: main is parked on a NotificationEvent
        // that is NOT signaled, AND no other thread is referencing the same
        // handle. Spell that out so the reader doesn't have to compare rows.
        if (mainTid != 0) {
            const WaiterRow* mainRow = nullptr;
            for (const auto& w : waiters) {
                if (w.tid == mainTid) { mainRow = &w; break; }
            }
            if (mainRow && mainRow->queried) {
                const bool sig = mainRow->signaled != 0;
                const auto h = mainRow->handle;
                std::size_t externalRefs = 0;
                for (const auto& w : waiters) {
                    if (w.tid == mainTid) continue;
                    if (w.rbx == h || w.rbp == h || w.rsi == h || w.rdi == h ||
                        w.r12 == h || w.r13 == h || w.r14 == h || w.r15 == h)
                        ++externalRefs;
                }
                a_os << std::format(
                    "  main TID {} waiting on HANDLE 0x{:x} [{}, {}]; "
                    "{} other waiters reference it.\n",
                    mainTid, h, EventTypeName(mainRow->eventType),
                    sig ? "SIGNALED" : "NOT signaled", externalRefs);
                if (!sig && externalRefs == 0) {
                    a_os << "  >>> classic dispatch+wait deadlock: nobody is "
                            "holding the producer side of main's handle.\n";
                }
            }
        }
    }

}
