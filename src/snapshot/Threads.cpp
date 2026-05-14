#include "PCH.h"
#include "snapshot/Threads.h"

#include "Config.h"
#include "Heartbeat.h"
#include "Symbols.h"

#include <TlHelp32.h>

#include <algorithm>
#include <vector>

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

            constexpr int kMaxFrames = 256;
            int frameNo = 0;

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
                a_os << std::format(
                    "    #{:02} 0x{:016x}  {}\n",
                    frameNo,
                    frame.AddrPC.Offset,
                    Symbols::ResolveLocked(static_cast<std::uintptr_t>(frame.AddrPC.Offset)));
                ++frameNo;
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
