#include "PCH.h"
#include "LeakedSpinLockBreaker.h"

#include "Config.h"
#include "SkyrimAnchors.h"
#include "Stats.h"

#include <format>
#include <limits>

#include <tlhelp32.h>

namespace WorkerSpinLockFix::LeakedSpinLockBreaker {

    namespace {

        // ----- config snapshot ----------------------------------------------
        bool          g_detect_only{ true };
        bool          g_diag{ false };
        std::uint32_t g_dwell_ms{ 5000 };
        std::uint32_t g_poll_ms{ 1000 };
        std::uint32_t g_recheck_ms{ 1500 };

        // ----- resolved anchors (copied at Install) -------------------------
        std::uintptr_t g_spinRets[SkyrimAnchors::Anchors::kMaxSpinRets] = {};
        std::size_t    g_spinCount{ 0 };

        // ----- watchdog -----------------------------------------------------
        std::thread       g_watchdog;
        std::atomic<bool> g_running{ false };

        // The watchdog only scans once the game has finished loading its
        // data (set at SKSE kDataLoaded). Before that the engine's threads
        // and address space are still being built up and any wild pointer
        // harvested from a half-initialised thread context is far more
        // likely; the load-time CTD in crash bucket CTD-7b4ecc21 faulted
        // here exactly that way. There is also nothing to recover during a
        // load screen, so staying idle until data-loaded removes the entire
        // window at no functional cost.
        std::atomic<bool> g_active{ false };

        std::uint64_t NowMs() noexcept { return ::GetTickCount64(); }

        // ----- memory-validated raw read ------------------------------------
        // True iff [a_addr, a_addr+a_size) lies entirely inside a single
        // committed, readable region. We validate with VirtualQuery BEFORE
        // dereferencing because SEH alone is not a reliable guard for a wild
        // read: during process teardown (the "exit to desktop CTD") or a
        // loader transition the exception dispatcher cannot always unwind a
        // hardware fault, so the access violation escapes as a hard crash
        // instead of being caught. VirtualQuery never dereferences the
        // pointer, so it is safe to call on arbitrary harvested values.
        bool IsReadable(std::uintptr_t a_addr, std::size_t a_size) noexcept {
            if (a_addr == 0 || a_size == 0) return false;
            if (a_addr > (std::numeric_limits<std::uintptr_t>::max)() - a_size) {
                return false;
            }
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_addr), &mbi,
                               sizeof(mbi)) == 0) {
                return false;
            }
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
            constexpr DWORD kReadable =
                PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                PAGE_EXECUTE_WRITECOPY;
            if ((mbi.Protect & kReadable) == 0) return false;
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto end  = base + mbi.RegionSize;
            return a_addr >= base && (a_addr + a_size) <= end;
        }

        // True iff the range is committed AND writable (for the force-release
        // interlocked store). Excludes read-only / copy-on-write pages.
        bool IsWritable(std::uintptr_t a_addr, std::size_t a_size) noexcept {
            if (a_addr == 0 || a_size == 0) return false;
            if (a_addr > (std::numeric_limits<std::uintptr_t>::max)() - a_size) {
                return false;
            }
            MEMORY_BASIC_INFORMATION mbi{};
            if (::VirtualQuery(reinterpret_cast<LPCVOID>(a_addr), &mbi,
                               sizeof(mbi)) == 0) {
                return false;
            }
            if (mbi.State != MEM_COMMIT) return false;
            if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
            constexpr DWORD kWritable =
                PAGE_READWRITE | PAGE_EXECUTE_READWRITE;
            if ((mbi.Protect & kWritable) == 0) return false;
            const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto end  = base + mbi.RegionSize;
            return a_addr >= base && (a_addr + a_size) <= end;
        }

        // VirtualQuery-validated, then SEH-guarded as a second line of
        // defence against a TOCTOU unmap between the query and the read.
        bool TryReadQword(std::uintptr_t a_addr, std::uintptr_t& a_out) noexcept {
            if (!IsReadable(a_addr, sizeof(std::uintptr_t))) return false;
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A plausible user-mode heap/data pointer (matches FreezeLogger's
        // MainWaitProbe heuristic): above the first 64 KiB, below the
        // 47-bit canonical user ceiling, 8-byte aligned.
        bool LooksLikePointer(std::uintptr_t a_v) noexcept {
            return a_v >= 0x0001'0000 &&
                   a_v <= 0x0000'7fff'ffff'ffffull &&
                   (a_v & 0x7) == 0;
        }

        // True iff *a_v reads as a BSSpinLock { uint32 owner; uint32 state }:
        // owner is 0 (free) or a plausible Windows TID (< 200000), state is
        // 0/1/2. Returns the decoded fields and the raw 64-bit word.
        bool LooksLikeLock(std::uintptr_t   a_v,
                           std::uint32_t&   a_owner,
                           std::uint32_t&   a_state,
                           std::uint64_t&   a_word) noexcept {
            if (!LooksLikePointer(a_v)) return false;
            std::uintptr_t pair = 0;
            if (!TryReadQword(a_v, pair)) return false;
            const auto owner = static_cast<std::uint32_t>(pair & 0xffffffff);
            const auto state = static_cast<std::uint32_t>(pair >> 32);
            if (!(owner == 0 || owner < 200000)) return false;
            if (state > 2) return false;
            a_owner = owner;
            a_state = state;
            a_word  = static_cast<std::uint64_t>(pair);
            return true;
        }

        // Lower-cased base name of the module that contains a_addr, or "" if
        // the address is not inside any loaded module. Address-based so it
        // works even where DbgHelp has no symbols (ntdll/KERNELBASE).
        std::string ModuleBaseNameForAddr(std::uintptr_t a_addr) {
            HMODULE hmod = nullptr;
            if (!::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_addr), &hmod) ||
                hmod == nullptr) {
                return {};
            }
            wchar_t path[MAX_PATH] = {};
            const DWORD n = ::GetModuleFileNameW(hmod, path, MAX_PATH);
            if (n == 0) return {};
            std::wstring w(path, n);
            const auto slash = w.find_last_of(L"\\/");
            const std::wstring base =
                (slash == std::wstring::npos) ? w : w.substr(slash + 1);
            std::string out;
            out.reserve(base.size());
            for (wchar_t c : base) {
                out.push_back(static_cast<char>(
                    std::tolower(static_cast<int>(c) & 0xff)));
            }
            return out;
        }

        // True for the system DLLs a thread's RIP sits in while parked in a
        // kernel wait (the syscall stub / wait wrapper lives here).
        bool IsSystemWaitModule(const std::string& a_name) {
            return a_name == "ntdll.dll"      ||
                   a_name == "kernelbase.dll" ||
                   a_name == "kernel32.dll"   ||
                   a_name == "win32u.dll"     ||
                   a_name == "user32.dll";
        }

        // ----- per-thread snapshot ------------------------------------------
        struct ThreadSnap {
            DWORD          tid{ 0 };
            bool           ok{ false };       // got a usable context
            std::uintptr_t rip{ 0 };
            std::uintptr_t rsp{ 0 };
            CONTEXT        ctx{};
        };

        std::vector<DWORD> EnumerateThreads(DWORD a_pid) {
            std::vector<DWORD> out;
            const HANDLE snap =
                ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return out;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID)
                            + sizeof(te.th32OwnerProcessID) &&
                        te.th32OwnerProcessID == a_pid) {
                        out.push_back(te.th32ThreadID);
                    }
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return out;
        }

        // Suspend a thread, capture its integer+control context, resume.
        ThreadSnap SnapshotThread(DWORD a_tid) {
            ThreadSnap s{};
            s.tid = a_tid;
            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!h) return s;
            if (::SuspendThread(h) == static_cast<DWORD>(-1)) {
                ::CloseHandle(h);
                return s;
            }
            s.ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            const bool got = ::GetThreadContext(h, &s.ctx) != FALSE;
            ::ResumeThread(h);
            ::CloseHandle(h);
            if (!got) return s;
            s.ok  = true;
            s.rip = static_cast<std::uintptr_t>(s.ctx.Rip);
            s.rsp = static_cast<std::uintptr_t>(s.ctx.Rsp);
            return s;
        }

        // Does a thread's stack window show a resolved spin-retry return
        // address? (i.e. it is contending a BSSpinLock right now.)
        bool ThreadIsSpinning(const ThreadSnap& a_t) {
            if (!a_t.ok) return false;
            for (std::uintptr_t off = 0; off < 0x100; off += 8) {
                std::uintptr_t v = 0;
                if (!TryReadQword(a_t.rsp + off, v)) break;
                for (std::size_t i = 0; i < g_spinCount; ++i) {
                    if (v == g_spinRets[i]) return true;
                }
            }
            return false;
        }

        // A BSSpinLock the victim is contending whose owner is someone else.
        struct LockRef {
            std::uintptr_t addr{ 0 };
            std::uint32_t  owner{ 0 };
            std::uint32_t  state{ 0 };
            std::uint64_t  word{ 0 };
        };

        // Scan the victim's integer regs + 1 KiB stack window for plausible
        // BSSpinLock pointers held by a DIFFERENT thread. De-duplicated by
        // address.
        void CollectForeignLocks(const ThreadSnap&      a_v,
                                 std::vector<LockRef>&  a_out) {
            const std::uintptr_t regs[] = {
                a_v.ctx.Rax, a_v.ctx.Rbx, a_v.ctx.Rcx, a_v.ctx.Rdx,
                a_v.ctx.Rsi, a_v.ctx.Rdi, a_v.ctx.R8,  a_v.ctx.R9,
                a_v.ctx.R10, a_v.ctx.R11, a_v.ctx.R12, a_v.ctx.R13,
                a_v.ctx.R14, a_v.ctx.R15,
            };
            auto consider = [&](std::uintptr_t v) {
                std::uint32_t o = 0, st = 0;
                std::uint64_t w = 0;
                if (!LooksLikeLock(v, o, st, w)) return;
                if (o == 0 || o == a_v.tid) return;   // free, or self-held
                if (st == 0) return;                   // not actually held
                for (const auto& e : a_out) {
                    if (e.addr == v) return;           // dedup
                }
                a_out.push_back({ v, o, st, w });
            };
            for (auto r : regs) consider(r);
            for (std::uintptr_t off = 0; off < 0x400; off += 8) {
                std::uintptr_t v = 0;
                if (!TryReadQword(a_v.rsp + off, v)) break;
                consider(v);
            }
        }

        // Force-release a leaked lock: clear it to 0 ONLY if it still holds
        // exactly the (owner,state) word we proved leaked, so we can never
        // clobber a lock that changed in the last instant. SEH-guarded.
        bool ForceRelease(std::uintptr_t a_lock, std::uint64_t a_expected) noexcept {
            if (!IsWritable(a_lock, sizeof(LONG64))) return false;
            __try {
                auto* p = reinterpret_cast<volatile LONG64*>(a_lock);
                const LONG64 prev = ::InterlockedCompareExchange64(
                    p, 0, static_cast<LONG64>(a_expected));
                return prev == static_cast<LONG64>(a_expected);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ----- candidate tracking across polls ------------------------------
        // A leaked-lock candidate must persist UNCHANGED across many polls
        // (firstSeenMs..dwell) before we even consider acting. Keyed by lock
        // address; reset whenever its owner/word/holder-RIP moves (the lock
        // is alive after all).
        struct Track {
            std::uint32_t  owner{ 0 };
            std::uint64_t  word{ 0 };
            std::uintptr_t ownerRip{ 0 };
            std::uint64_t  firstSeenMs{ 0 };
            std::uint64_t  lastPoll{ 0 };
            bool           handled{ false };
        };
        std::unordered_map<std::uintptr_t, Track> g_tracks;

        // A confirmed candidate this poll: victim + the foreign lock + the
        // parked owner snapshot.
        struct Candidate {
            DWORD          victim{ 0 };
            LockRef        lock{};
            DWORD          owner{ 0 };
            std::uintptr_t ownerRip{ 0 };
        };

        // Re-prove the leak immediately before acting: the owner is STILL
        // parked at the same RIP in a system wait module, and the lock still
        // holds the same (owner,state) word. Returns true iff still leaked.
        bool ReproveLeak(const Candidate& a_c) {
            const auto os = SnapshotThread(a_c.owner);
            if (!os.ok) return false;
            if (os.rip != a_c.ownerRip) return false;
            if (!IsSystemWaitModule(ModuleBaseNameForAddr(os.rip))) return false;
            if (ThreadIsSpinning(os)) return false;
            std::uint32_t o = 0, st = 0;
            std::uint64_t w = 0;
            if (!LooksLikeLock(a_c.lock.addr, o, st, w)) return false;
            return o == a_c.owner && st != 0 && w == a_c.lock.word;
        }

        void HandleConfirmed(const Candidate& a_c, std::uint64_t a_dwellMs) {
            Stats::OnLeakedLockStuck();

            if (g_detect_only) {
                logs::warn(
                    "[LeakedSpinLockBreaker] LEAKED BSSpinLock detected "
                    "(detect_only): TID {} is spinning on BSSpinLock 0x{:x} "
                    "(owner={}, state={}) that has been held UNCHANGED for "
                    "{} ms by TID {}, which is parked in a kernel wait "
                    "(RIP 0x{:x}, {}) inside the idle worker pool and will "
                    "never release it -- the unlock was leaked. This is the "
                    "case-study 30 leaked-lock freeze; SetEvent cannot help. "
                    "WOULD force-release the lock here; set "
                    "[leaked_spinlock_breaker] detect_only=false to actually "
                    "recover.",
                    a_c.victim, a_c.lock.addr, a_c.lock.owner, a_c.lock.state,
                    a_dwellMs, a_c.owner, a_c.ownerRip,
                    ModuleBaseNameForAddr(a_c.ownerRip));
                return;
            }

            if (ForceRelease(a_c.lock.addr, a_c.lock.word)) {
                Stats::OnLeakedLockReleased();
                logs::warn(
                    "[LeakedSpinLockBreaker] LEAKED BSSpinLock RELEASED: "
                    "force-released lock 0x{:x} (was owner={}, state={}, held "
                    "{} ms by parked TID {}); TID {} should resume this frame. "
                    "The holder was provably parked in a kernel wait and will "
                    "never reference this lock again, so the clear is safe.",
                    a_c.lock.addr, a_c.lock.owner, a_c.lock.state, a_dwellMs,
                    a_c.owner, a_c.victim);
            } else {
                logs::warn(
                    "[LeakedSpinLockBreaker] leaked-lock force-release on "
                    "0x{:x} stood down: the lock word changed between proof "
                    "and the interlocked clear (it was not actually leaked, "
                    "or another agent released it). No action taken.",
                    a_c.lock.addr);
            }
        }

        // ----- watchdog -----------------------------------------------------
        void WatchdogLoop() {
            std::uint64_t pollno = 0;
            const DWORD   pid    = ::GetCurrentProcessId();
            const DWORD   selfTid = ::GetCurrentThreadId();

            while (g_running.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(g_poll_ms));
                if (!g_running.load(std::memory_order_relaxed)) break;

                // Stay idle (no thread suspension, no memory probing) until
                // the game has finished loading its data. Scanning during the
                // load/menu phase is both pointless (no gameplay freeze to
                // break) and the riskiest window for harvesting a wild pointer
                // out of a half-built thread context.
                if (!g_active.load(std::memory_order_acquire)) continue;
                ++pollno;

                // Snapshot every thread once (suspend/getcontext/resume).
                const auto tids = EnumerateThreads(pid);
                std::vector<ThreadSnap> snaps;
                snaps.reserve(tids.size());
                std::unordered_map<DWORD, std::size_t> idx;
                for (DWORD tid : tids) {
                    if (tid == selfTid) continue;
                    auto s = SnapshotThread(tid);
                    idx[tid] = snaps.size();
                    snaps.push_back(std::move(s));
                }

                auto ownerSnap = [&](DWORD tid) -> const ThreadSnap* {
                    const auto it = idx.find(tid);
                    if (it == idx.end()) return nullptr;
                    return &snaps[it->second];
                };

                // Find this poll's leaked-lock candidates: a spinning victim
                // contending a lock whose owner is a parked, non-spinning
                // thread in a kernel wait.
                std::vector<Candidate> cands;
                for (const auto& v : snaps) {
                    if (!ThreadIsSpinning(v)) continue;
                    std::vector<LockRef> locks;
                    CollectForeignLocks(v, locks);
                    for (const auto& L : locks) {
                        const ThreadSnap* o = ownerSnap(L.owner);
                        if (o == nullptr || !o->ok) continue;
                        if (ThreadIsSpinning(*o)) continue;  // owner mid-acquire
                        if (!IsSystemWaitModule(ModuleBaseNameForAddr(o->rip)))
                            continue;                         // owner not parked
                        bool dup = false;
                        for (const auto& c : cands) {
                            if (c.lock.addr == L.addr) { dup = true; break; }
                        }
                        if (dup) continue;
                        cands.push_back({ v.tid, L, L.owner, o->rip });
                    }
                }

                const auto now = NowMs();

                // Update the persistence map and act on anything that has
                // been steady-state leaked for the full dwell window.
                for (const auto& c : cands) {
                    auto& t = g_tracks[c.lock.addr];
                    const bool fresh = (t.firstSeenMs == 0);
                    const bool moved = !fresh &&
                        (t.owner != c.owner || t.word != c.lock.word ||
                         t.ownerRip != c.ownerRip);
                    if (fresh || moved) {
                        t.owner       = c.owner;
                        t.word        = c.lock.word;
                        t.ownerRip    = c.ownerRip;
                        t.firstSeenMs = now;
                        t.handled     = false;
                    }
                    t.lastPoll = pollno;

                    if (t.handled) continue;
                    const auto dwell = now - t.firstSeenMs;
                    if (dwell < g_dwell_ms) {
                        if (g_diag) {
                            logs::info(
                                "[LeakedSpinLockBreaker.diag] candidate lock "
                                "0x{:x} owner={} held {} ms (victim {}); "
                                "waiting for dwell {} ms.",
                                c.lock.addr, c.owner, dwell, c.victim,
                                g_dwell_ms);
                        }
                        continue;
                    }

                    // Steady-state confirmation window: re-prove the leak
                    // after a pause. Anything that moves stands us down.
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(g_recheck_ms));
                    if (!g_running.load(std::memory_order_relaxed)) return;
                    if (!ReproveLeak(c)) {
                        if (g_diag) {
                            logs::info(
                                "[LeakedSpinLockBreaker.diag] lock 0x{:x} "
                                "changed across the recheck window; not "
                                "leaked, standing by.",
                                c.lock.addr);
                        }
                        t.firstSeenMs = NowMs();   // restart the dwell clock
                        continue;
                    }

                    t.handled = true;
                    HandleConfirmed(c, NowMs() - t.firstSeenMs);
                }

                // Drop tracks whose lock no longer qualifies (released /
                // owner moved / victim resumed): not seen this poll.
                for (auto it = g_tracks.begin(); it != g_tracks.end();) {
                    if (it->second.lastPoll != pollno) {
                        it = g_tracks.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }

    } // namespace

    bool Install() {
        const auto& cfg = Config::Get();
        if (!cfg.lsb_enabled) {
            logs::info(
                "[LeakedSpinLockBreaker] disabled by config "
                "(leaked_spinlock_breaker.enabled = false).");
            return false;
        }

        if (!SkyrimAnchors::AvailableSpinRetry()) {
            logs::warn(
                "[LeakedSpinLockBreaker] BSSpinLock spin-retry site not "
                "resolved; module will NOT arm.");
            return false;
        }

        const auto& a = SkyrimAnchors::Get();
        g_spinCount = a.spinRetCount;
        for (std::size_t i = 0; i < g_spinCount; ++i) {
            g_spinRets[i] = a.spinRets[i];
        }

        g_detect_only = cfg.lsb_detect_only;
        g_diag        = cfg.lsb_diagnostic_logging;
        g_dwell_ms    = cfg.lsb_dwell_threshold_ms;
        g_poll_ms     = (cfg.lsb_poll_interval_ms == 0) ? 1000
                                                        : cfg.lsb_poll_interval_ms;
        g_recheck_ms  = cfg.lsb_recheck_window_ms;

        g_running.store(true, std::memory_order_relaxed);
        g_watchdog = std::thread(WatchdogLoop);
        g_watchdog.detach();

        logs::info(
            "[LeakedSpinLockBreaker] armed (idle until data-loaded). {} "
            "spin-retry site(s) tracked. mode={}, dwell_ms={}, poll_ms={}, "
            "recheck_ms={}, diag={}.",
            g_spinCount,
            g_detect_only ? "DETECT-ONLY" : "ACTIVE (will force-release)",
            g_dwell_ms, g_poll_ms, g_recheck_ms, g_diag ? "ON" : "OFF");
        return true;
    }

    void OnDataLoaded() {
        if (!g_active.exchange(true, std::memory_order_release)) {
            logs::info(
                "[LeakedSpinLockBreaker] data loaded; watchdog scanning is "
                "now active.");
        }
    }

    void Stop() {
        g_running.store(false, std::memory_order_relaxed);
        g_active.store(false, std::memory_order_relaxed);
    }

}
