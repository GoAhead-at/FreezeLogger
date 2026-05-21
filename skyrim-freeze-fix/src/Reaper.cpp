#include "PCH.h"
#include "Reaper.h"
#include "AcquireHook.h"
#include "Config.h"
#include "Stats.h"

#include <TlHelp32.h>
#include <Psapi.h>

// =============================================================================
// Stale-owner reaper.
//
// Safety net under the AcquireHook + WaitGraph + Breaker pipeline. The
// reaper picks up cases the entry-point hook can never see: threads that
// died holding a lock, observed wait edges where the owner has become
// invalid, etc.
//
// The reaper does not perform cycle detection or live-cycle break. It
// acts only on dead-owner cases. Live cycles are the AcquireHook +
// Breaker path. The poll interval is config-driven via
// Config::reaper_interval_ms.
// =============================================================================

namespace WorkerSpinLockFix::Reaper {

    namespace {

        struct ThreadContext {
            DWORD          tid{ 0 };
            std::uintptr_t rip{ 0 };
            std::uintptr_t rsp{ 0 };
            std::uintptr_t rax{ 0 };
            std::uintptr_t rbx{ 0 };
            std::uintptr_t rcx{ 0 };
            std::uintptr_t rdx{ 0 };
            std::uintptr_t rsi{ 0 };
            std::uintptr_t rdi{ 0 };
            std::uintptr_t r8{ 0 };
            std::uintptr_t r9{ 0 };
            std::uintptr_t r10{ 0 };
            std::uintptr_t r11{ 0 };
            std::uintptr_t r12{ 0 };
            std::uintptr_t r13{ 0 };
            std::uintptr_t r14{ 0 };
            std::uintptr_t r15{ 0 };
            bool           ok{ false };
        };

        struct LockCandidate {
            std::uintptr_t addr{ 0 };
            std::uint32_t  owner{ 0 };
            std::uint32_t  state{ 0 };
            const char*    source{ nullptr };
        };

        struct ObservedEdge {
            DWORD          waiter{ 0 };
            std::uintptr_t lock_addr{ 0 };
            std::uint32_t  owner{ 0 };
            std::uint32_t  state{ 0 };
            const char*    source{ nullptr };
        };

        struct StableEdge {
            ObservedEdge   edge{};
            std::uint64_t  first_seen_ms{ 0 };
            std::uint64_t  last_probe_log_ms{ 0 };
            bool           seen_this_tick{ false };
        };

        std::atomic<bool> g_stop{ false };
        std::atomic<bool> g_started{ false };
        std::thread       g_thread;

        std::vector<StableEdge> g_edges;

        constexpr std::uint64_t kStaleStableMs   = 2000;
        // Live-owner deadlock probe. After this many ms with the same
        // (waiter, lock, owner) edge holding, log a diagnostic line so we
        // can see deadlocks the AcquireHook+WaitGraph cycle detector
        // cannot see (e.g. owner is blocked on a non-BSSpinLock primitive
        // such as a critical section, event, or inlined acquire path).
        constexpr std::uint64_t kLiveProbeMs     = 5000;
        constexpr std::uint64_t kLiveProbeRepeat = 30000;

        bool TryReadQword(std::uintptr_t addr, std::uintptr_t& out) noexcept {
            __try {
                out = *reinterpret_cast<volatile std::uintptr_t*>(addr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                out = 0;
                return false;
            }
        }

        bool TryReadSpinLock(
            std::uintptr_t addr,
            std::uint32_t& owner,
            std::uint32_t& state) noexcept
        {
            std::uintptr_t pair = 0;
            if (!TryReadQword(addr, pair)) {
                return false;
            }
            owner = static_cast<std::uint32_t>(pair & 0xffffffff);
            state = static_cast<std::uint32_t>(pair >> 32);
            return true;
        }

        bool LooksLikePointer(std::uintptr_t value) noexcept {
            return value >= 0x10000 && value < 0x0000800000000000;
        }

        bool LooksLikeLock(
            std::uintptr_t addr,
            std::uint32_t& owner,
            std::uint32_t& state) noexcept
        {
            if (!LooksLikePointer(addr)) {
                return false;
            }
            if ((addr & 0x3) != 0) {
                return false;
            }
            if (!TryReadSpinLock(addr, owner, state)) {
                return false;
            }
            const bool owner_ok = owner == 0 || owner < 200000;
            const bool state_ok = state <= 2;
            return owner_ok && state_ok;
        }

        std::vector<DWORD> EnumerateThreads() {
            std::vector<DWORD> tids;
            const DWORD pid = ::GetCurrentProcessId();
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) {
                return tids;
            }

            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid) {
                        tids.push_back(te.th32ThreadID);
                    }
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return tids;
        }

        ThreadContext SnapshotThread(DWORD tid) {
            ThreadContext out{};
            out.tid = tid;

            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE,
                tid);
            if (!h) {
                return out;
            }

            const DWORD prev = ::SuspendThread(h);
            if (prev == static_cast<DWORD>(-1)) {
                ::CloseHandle(h);
                return out;
            }

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            if (::GetThreadContext(h, &ctx)) {
                out.rip = static_cast<std::uintptr_t>(ctx.Rip);
                out.rsp = static_cast<std::uintptr_t>(ctx.Rsp);
                out.rax = static_cast<std::uintptr_t>(ctx.Rax);
                out.rbx = static_cast<std::uintptr_t>(ctx.Rbx);
                out.rcx = static_cast<std::uintptr_t>(ctx.Rcx);
                out.rdx = static_cast<std::uintptr_t>(ctx.Rdx);
                out.rsi = static_cast<std::uintptr_t>(ctx.Rsi);
                out.rdi = static_cast<std::uintptr_t>(ctx.Rdi);
                out.r8  = static_cast<std::uintptr_t>(ctx.R8);
                out.r9  = static_cast<std::uintptr_t>(ctx.R9);
                out.r10 = static_cast<std::uintptr_t>(ctx.R10);
                out.r11 = static_cast<std::uintptr_t>(ctx.R11);
                out.r12 = static_cast<std::uintptr_t>(ctx.R12);
                out.r13 = static_cast<std::uintptr_t>(ctx.R13);
                out.r14 = static_cast<std::uintptr_t>(ctx.R14);
                out.r15 = static_cast<std::uintptr_t>(ctx.R15);
                out.ok = true;
            }

            ::ResumeThread(h);
            ::CloseHandle(h);
            return out;
        }

        bool IsThreadAlive(std::uint32_t tid) {
            if (tid == 0) {
                return false;
            }
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h) {
                return false;
            }
            DWORD code = 0;
            const BOOL ok = ::GetExitCodeThread(h, &code);
            ::CloseHandle(h);
            return ok && code == STILL_ACTIVE;
        }

        bool IsSpinner(const ThreadContext& ctx, std::uintptr_t spin_retry) {
            if (!ctx.ok || spin_retry == 0) {
                return false;
            }
            if (ctx.rip == spin_retry) {
                return true;
            }
            for (std::uintptr_t off = 0; off < 0x100; off += 8) {
                std::uintptr_t value = 0;
                if (!TryReadQword(ctx.rsp + off, value)) {
                    break;
                }
                if (value == spin_retry) {
                    return true;
                }
            }
            return false;
        }

        void AddCandidate(
            std::vector<LockCandidate>& out,
            std::uintptr_t addr,
            const char* source)
        {
            std::uint32_t owner = 0;
            std::uint32_t state = 0;
            if (!LooksLikeLock(addr, owner, state)) {
                return;
            }
            const auto it = std::find_if(out.begin(), out.end(),
                [addr](const LockCandidate& c) { return c.addr == addr; });
            if (it == out.end()) {
                out.push_back({ addr, owner, state, source });
            }
        }

        std::vector<LockCandidate> CollectLockCandidates(const ThreadContext& ctx) {
            std::vector<LockCandidate> out;
            const struct { const char* name; std::uintptr_t value; } regs[] = {
                { "RAX", ctx.rax }, { "RBX", ctx.rbx }, { "RCX", ctx.rcx },
                { "RDX", ctx.rdx }, { "RSI", ctx.rsi }, { "RDI", ctx.rdi },
                { "R8",  ctx.r8  }, { "R9",  ctx.r9  }, { "R10", ctx.r10 },
                { "R11", ctx.r11 }, { "R12", ctx.r12 }, { "R13", ctx.r13 },
                { "R14", ctx.r14 }, { "R15", ctx.r15 },
            };
            for (const auto& reg : regs) {
                AddCandidate(out, reg.value, reg.name);
            }

            for (std::uintptr_t off = 0; off < 0x400; off += 8) {
                std::uintptr_t value = 0;
                if (!TryReadQword(ctx.rsp + off, value)) {
                    break;
                }
                AddCandidate(out, value, "stack");
            }
            return out;
        }

        bool ForceRelease(std::uintptr_t lock_addr, std::uint32_t observed_state) {
            auto* state_ptr = reinterpret_cast<volatile LONG*>(lock_addr + 4);
            const LONG prev = ::InterlockedCompareExchange(
                state_ptr, 0, static_cast<LONG>(observed_state));
            return prev == static_cast<LONG>(observed_state);
        }

        bool SameEdge(const ObservedEdge& a, const ObservedEdge& b) {
            return a.waiter == b.waiter &&
                   a.lock_addr == b.lock_addr &&
                   a.owner == b.owner;
        }

        void UpdateStableEdges(
            const std::vector<ObservedEdge>& observed,
            std::uint64_t now_ms)
        {
            for (auto& existing : g_edges) {
                existing.seen_this_tick = false;
            }

            for (const auto& edge : observed) {
                auto it = std::find_if(g_edges.begin(), g_edges.end(),
                    [&edge](const StableEdge& s) { return SameEdge(s.edge, edge); });
                if (it == g_edges.end()) {
                    g_edges.push_back({ edge, now_ms, 0, true });
                } else {
                    it->edge = edge;
                    it->seen_this_tick = true;
                }
            }

            g_edges.erase(std::remove_if(g_edges.begin(), g_edges.end(),
                [](const StableEdge& s) { return !s.seen_this_tick; }),
                g_edges.end());
        }

        struct OwnerSnapshot {
            std::uintptr_t rip{ 0 };
            std::uintptr_t rcx{ 0 };
            bool           ok{ false };
            bool           is_spinner{ false };
        };

        OwnerSnapshot ProbeOwner(DWORD owner_tid, std::uintptr_t spin_retry) {
            OwnerSnapshot out{};
            const auto ctx = SnapshotThread(owner_tid);
            if (!ctx.ok) {
                return out;
            }
            out.rip = ctx.rip;
            out.rcx = ctx.rcx;
            out.ok  = true;
            out.is_spinner = IsSpinner(ctx, spin_retry);
            return out;
        }

        struct ModuleAt {
            std::array<char, 64> name{};
            std::uintptr_t       base{ 0 };
        };

        ModuleAt ResolveModule(std::uintptr_t addr) {
            ModuleAt out{};
            if (addr == 0) {
                return out;
            }
            HMODULE mod = nullptr;
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(addr),
                    &mod) ||
                !mod)
            {
                return out;
            }
            out.base = reinterpret_cast<std::uintptr_t>(mod);
            wchar_t wbuf[MAX_PATH] = {};
            DWORD n = ::GetModuleBaseNameW(::GetCurrentProcess(), mod, wbuf, MAX_PATH);
            for (DWORD i = 0; i < n && i + 1 < out.name.size(); ++i) {
                out.name[i] = static_cast<char>(wbuf[i] & 0x7f);
            }
            return out;
        }

        void LogLiveOwnerProbes(std::uintptr_t spin_retry, std::uint64_t now_ms) {
            for (auto& stable : g_edges) {
                if (stable.edge.owner == 0 || !IsThreadAlive(stable.edge.owner)) {
                    continue;
                }
                const auto held_ms = (now_ms < stable.first_seen_ms)
                    ? 0ull
                    : now_ms - stable.first_seen_ms;
                if (held_ms < kLiveProbeMs) {
                    continue;
                }
                if (stable.last_probe_log_ms != 0 &&
                    now_ms - stable.last_probe_log_ms < kLiveProbeRepeat)
                {
                    continue;
                }
                stable.last_probe_log_ms = now_ms;

                const auto snap = ProbeOwner(stable.edge.owner, spin_retry);
                const auto mod  = ResolveModule(snap.rip);
                const auto rva  = (mod.base != 0 && snap.rip >= mod.base)
                    ? snap.rip - mod.base
                    : 0;

                logs::warn(
                    "[REAPER] LIVE-OWNER WAIT held={}ms waiter=TID {} "
                    "lock=0x{:x} (state={}) owner=TID {} owner_rip=0x{:x} "
                    "({}+0x{:x}) owner_rcx=0x{:x} owner_is_spinner={}",
                    held_ms,
                    stable.edge.waiter,
                    stable.edge.lock_addr,
                    stable.edge.state,
                    stable.edge.owner,
                    snap.rip,
                    mod.name.data(),
                    rva,
                    snap.rcx,
                    snap.is_spinner);
            }
        }

        void ReapStaleObservedLocks(std::uint64_t now_ms) {
            for (auto& stable : g_edges) {
                if (stable.edge.owner == 0 || IsThreadAlive(stable.edge.owner)) {
                    continue;
                }
                if (now_ms < stable.first_seen_ms ||
                    now_ms - stable.first_seen_ms < kStaleStableMs)
                {
                    continue;
                }
                if (ForceRelease(stable.edge.lock_addr, stable.edge.state)) {
                    Stats::OnStaleReaped();
                    logs::warn(
                        "[REAPER] force-released observed stale BSSpinLock "
                        "0x{:x} (waiter TID {}, dead owner TID {}, state={}, "
                        "stable={}ms).",
                        stable.edge.lock_addr,
                        stable.edge.waiter,
                        stable.edge.owner,
                        stable.edge.state,
                        now_ms - stable.first_seen_ms);
                } else {
                    Stats::OnForceRace();
                }
            }
        }

        void Tick(std::uintptr_t spin_retry) {
            const DWORD self_tid = ::GetCurrentThreadId();
            const auto tids = EnumerateThreads();
            std::vector<ObservedEdge> observed;
            std::size_t spinners = 0;
            std::size_t candidates = 0;

            for (const DWORD tid : tids) {
                if (tid == self_tid) {
                    continue;
                }
                const auto ctx = SnapshotThread(tid);
                if (!IsSpinner(ctx, spin_retry)) {
                    continue;
                }
                ++spinners;
                const auto locks = CollectLockCandidates(ctx);
                candidates += locks.size();
                for (const auto& lock : locks) {
                    if (lock.state == 0 || lock.owner == 0 || lock.owner == tid) {
                        continue;
                    }
                    observed.push_back({
                        tid,
                        lock.addr,
                        lock.owner,
                        lock.state,
                        lock.source
                    });
                }
            }

            const auto now_ms = ::GetTickCount64();
            UpdateStableEdges(observed, now_ms);
            Stats::OnReaperScan(tids.size(), spinners, candidates, g_edges.size());
            ReapStaleObservedLocks(now_ms);
            LogLiveOwnerProbes(spin_retry, now_ms);
        }

        void ReaperBody(std::uintptr_t spin_retry, std::uint32_t interval_ms) {
            logs::info(
                "[REAPER] thread starting (interval={}ms, stale_stable={}ms, "
                "spin_retry=0x{:x}).",
                interval_ms, kStaleStableMs, spin_retry);

            const auto interval = std::chrono::milliseconds(interval_ms);
            while (!g_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(interval);
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                Tick(spin_retry);
            }

            logs::info("[REAPER] thread exiting.");
        }

    } // namespace

    bool Install() {
        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        const auto& cfg = Config::Get();
        const auto spin_retry = AcquireHook::SpinRetryAddress();
        if (spin_retry == 0) {
            logs::warn(
                "[REAPER] spin-retry address not resolved (AcquireHook may "
                "have failed). Reaper will not start.");
            g_started.store(false, std::memory_order_release);
            return false;
        }

        g_stop.store(false, std::memory_order_relaxed);
        try {
            g_thread = std::thread(ReaperBody, spin_retry, cfg.reaper_interval_ms);
            g_thread.detach();
        } catch (const std::exception& e) {
            logs::critical("[REAPER] failed to start: {}", e.what());
            g_started.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }

    void Stop() {
        g_stop.store(true, std::memory_order_relaxed);
    }

}
