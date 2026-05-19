#include "PCH.h"
#include "Hooks.h"
#include "Config.h"
#include "Stats.h"

// =============================================================================
// WorkerSpinLockFix v0.13 - Approach G (narrowed) + Approach F (safety net).
//
// Why v0.13 narrows v0.12:
//   v0.12 wrapped EVERY entry to id_40706 in our shared mutex. The
//   first 3-minute test session showed:
//
//       id_40706 entries=21004 contended=3676  (~117/sec, ~17.5% wait)
//       id_19369 entries=7 recursive=23 contended=0
//
//   id_40706 is called on per-object instances - its lock is at
//   `[this + 0x150]`, not at a fixed RVA. Different `this` pointers
//   carry different locks. Only when `this + 0x150` happens to equal
//   the global LockB at RVA 0x2f3b8e8 is the call on the AB-BA path
//   the case study identified. The other ~99 % of calls are
//   per-object instances the engine deliberately runs concurrently
//   on multiple worker threads. v0.12 forced all of them through
//   one mutex, stretched worker processing windows, and the timing
//   change exposed a race in scene-graph code that does NOT crash
//   without our plugin installed (user-confirmed).
//
//   v0.13 hooks id_40706 the same way at every call site, but the
//   wrapper checks `(this + 0x150) == &LockB` at runtime. Only the
//   matching invocations take our mutex. The rest pass straight
//   through to the original. This preserves the structural fix for
//   the AB-BA-prone path while giving the engine its concurrency
//   back for unrelated per-object processing.
//
// Approach G (unchanged from v0.12 in spirit):
//   Single shared std::recursive_mutex serialises:
//     - every entry to id 19369 (its LockA is a fixed-RVA global,
//       always on the AB-BA path; ~30 invocations / 3 min in v0.12,
//       so contention cost is irrelevant either way)
//     - id 40706 invocations whose this+0x150 == &LockB only.
//
// What v0.1-v0.8 got wrong, and why this is different:
//
//   v0.1-v0.8 wrapped the calls at the engine call sites and expected
//   acquire/release brackets to balance perfectly. They didn't: the
//   engine had tail-jump exits and reentry through paths the wrapper
//   didn't account for, and each unbalanced exit leaked the mutex.
//
//   v0.12 puts the std::scoped_lock inside the hook function itself.
//   The original (id 19369 or id 40706) is then called with the lock
//   held, and the lock is released by the scoped_lock destructor when
//   our hook function returns. The original's internal exits don't
//   matter: they all eventually return to our hook, and the hook
//   has exactly one return point per function (RAII over normal
//   control flow only; SEH out of an engine function is an entirely
//   different problem we don't try to solve).
//
//   Static disassembly confirmed:
//     - id 19369 has a single `ret` (at +0x697). All internal jumps
//       converge there.
//     - id 40706 has a single `ret` (at +0x3ed).
//     - id 19369 calls itself recursively at +0x9d. The recursion is
//       handled by std::recursive_mutex without special-casing.
//     - id 40706 internally descends through id 37388 -> id 36854 ->
//       id 19369. That recursion is also handled by the recursive
//       mutex (same thread re-acquires).
//
// Approach F (the v0.11 stale-owner reaper) is retained as a safety
// net underneath. It is independent of Approach G: it has no hooks,
// just one background thread that reads the engine's LockA / LockB
// state every 250 ms and force-releases via CAS if either lock has
// been held for >2 s by a thread that no longer exists in the process.
// In normal v0.12 operation Approach G should make the reaper's
// trigger condition impossible (no thread takes LockA and exits while
// holding it, because no thread can hold LockA while another waits on
// it), but the cost of the reaper is essentially zero so we keep it
// as defence in depth.
//
// Function prototypes derived from disassembly:
//
//   id 19369 takes 6 arguments, returns a byte (bool):
//     RCX:        void*     - object pointer (saved into RSI)
//     RDX:        void*     - object pointer (saved into RDI)
//     R8B:        uint8_t   - byte flag (zero-extended via movzx)
//     R9:         void*     - pointer (saved at [rsp+0x20] home)
//     stack +0x28 callee-view: uint32_t  - read at [rbp+0x77]
//     stack +0x30 callee-view: uint8_t   - read at [rbp+0x7f]
//   Returns: AL (zero-extended into EAX at +0x682: `movzx eax, bl`).
//
//   id 40706 takes a single argument, returns void:
//     RCX: void* - this pointer (saved into R14)
//     The body computes [r14 + 0x150] as the BSSpinLock pointer it
//     acquires.
//
// Coexistence with skyrim-freeze-fix.dll (which entry-point patches
// id 12210 / id 66983) is automatic. Our hooks are at call sites of
// id 19369 / id 40706, not at BSSpinLock primitives. The two plugins
// hook completely disjoint addresses.
// =============================================================================

namespace WorkerSpinLockFix::Hooks {

    namespace {

        // ---- Approach G: serialised entry to id 19369 / id 40706 ----------

        // One global recursive_mutex shared between BOTH hook functions.
        //
        // Sharing is essential. The freeze topology has thread T inside
        // id 40706 (LockB held) descending into id 19369 (where LockA
        // would be acquired). If id 19369 had its own mutex, thread T
        // would block on it while still holding the id_40706 mutex; a
        // different thread already inside id 19369 would block on the
        // id_40706 mutex; deadlock pattern reformed at the wrapper
        // layer. With one shared mutex, T's recursive entry into
        // id_19369 just bumps the recursion count.
        std::recursive_mutex g_section;

        // Cached absolute address of the global LockB. Filled in by
        // Install() before any hook can possibly fire (we set it before
        // patching the call sites). Read by Hook_40706 to decide
        // whether the current invocation is on the AB-BA path.
        std::uintptr_t g_lockB_addr_for_narrow = 0;

        // Engine function pointers. Filled in by Install().
        using Fn19369 = bool (__fastcall*)(
            void*       a /*RCX*/,
            void*       b /*RDX*/,
            std::uint8_t c /*R8B*/,
            void*       d /*R9*/,
            std::uint32_t e /*stack 5th*/,
            std::uint8_t  f /*stack 6th*/);
        using Fn40706 = void (__fastcall*)(void* self /*RCX*/);

        Fn19369 g_orig_19369 = nullptr;
        Fn40706 g_orig_40706 = nullptr;

        // Per-thread current depth, used only to discriminate "first
        // entry by this thread" (-> Stats::OnEntry) from "recursive
        // entry by this thread" (-> Stats::OnRecursiveEntry). The
        // mutex itself does the actual mutual exclusion; this is a
        // pure observability counter.
        thread_local std::uint32_t tls_depth = 0;

        // try_lock probe used to determine whether our acquire is
        // contended. We try non-blocking first; if it fails, we count
        // a contention event and fall back to a blocking acquire.
        // try_lock on a recursive_mutex held by the SAME thread always
        // succeeds, so this also correctly elides the "contention"
        // counter on recursive re-entry by the holding thread.
        struct ContentionAwareLock {
            std::recursive_mutex& m;
            bool                  recursive;
            bool                  contended;

            ContentionAwareLock(std::recursive_mutex& mu, std::string_view which)
                : m(mu), recursive(false), contended(false)
            {
                if (m.try_lock()) {
                    // Acquired without waiting. Could be a fresh top-level
                    // entry or a recursive re-entry by the same thread.
                    recursive = (tls_depth > 0);
                } else {
                    // Held by another thread.
                    contended = true;
                    Stats::OnContended(which);
                    m.lock();
                    recursive = (tls_depth > 0);
                }
                if (recursive) {
                    Stats::OnRecursiveEntry(which);
                } else {
                    Stats::OnEntry(which);
                }
                ++tls_depth;
            }

            ~ContentionAwareLock() {
                --tls_depth;
                m.unlock();
            }

            ContentionAwareLock(const ContentionAwareLock&) = delete;
            ContentionAwareLock& operator=(const ContentionAwareLock&) = delete;
        };

        // ---- Hook bodies --------------------------------------------------
        bool __fastcall Hook_19369(
            void* a, void* b, std::uint8_t c, void* d,
            std::uint32_t e, std::uint8_t f)
        {
            ContentionAwareLock guard(g_section, "id_19369");
            return g_orig_19369(a, b, c, d, e, f);
        }

        void __fastcall Hook_40706(void* self)
        {
            // The function takes its lock at `this + 0x150`. Only when
            // that address equals the global LockB is the invocation
            // on the AB-BA-prone path. For everything else, pass
            // through to preserve the engine's intended concurrency
            // across per-object instances.
            const auto lock_addr =
                reinterpret_cast<std::uintptr_t>(self) + 0x150;
            if (lock_addr != g_lockB_addr_for_narrow) {
                Stats::OnPassthrough("id_40706");
                g_orig_40706(self);
                return;
            }
            ContentionAwareLock guard(g_section, "id_40706");
            g_orig_40706(self);
        }

        // ---- Approach F: stale-owner reaper (carried over from v0.11) -----

        constexpr std::uintptr_t kLockA_RVA = 0x2eff8e0;
        constexpr std::uintptr_t kLockB_RVA = 0x2f3b8e8;

        struct LockTarget {
            std::uintptr_t addr            = 0;
            const char*    name            = nullptr;
            std::uint32_t  prev_owner      = 0;
            std::uint32_t  prev_state      = 0;
            std::uint64_t  stable_since_ms = 0;
        };

        LockTarget g_targets[2] = {
            { 0, "LockA" },
            { 0, "LockB" },
        };

        std::atomic<bool> g_reaper_stop{ false };
        std::atomic<bool> g_reaper_started{ false };
        std::thread       g_reaper_thread;

        constexpr auto         kCheckInterval     = std::chrono::milliseconds(250);
        constexpr std::uint64_t kStaleThresholdMs = 2000;

        inline std::uint32_t ReadOwner(std::uintptr_t addr) {
            return *reinterpret_cast<const volatile std::uint32_t*>(addr);
        }
        inline std::uint32_t ReadState(std::uintptr_t addr) {
            return *reinterpret_cast<const volatile std::uint32_t*>(addr + 4);
        }

        bool IsThreadAlive(std::uint32_t tid) {
            if (tid == 0) return false;
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
                                    FALSE, tid);
            if (h == nullptr) return false;
            DWORD code = 0;
            BOOL  ok   = ::GetExitCodeThread(h, &code);
            ::CloseHandle(h);
            if (!ok) return false;
            return code == STILL_ACTIVE;
        }

        bool ForceRelease(LockTarget& tgt, std::uint32_t observed_state) {
            auto* state_ptr =
                reinterpret_cast<volatile LONG*>(tgt.addr + 4);
            const LONG prev = ::InterlockedCompareExchange(
                state_ptr, 0, static_cast<LONG>(observed_state));
            return prev == static_cast<LONG>(observed_state);
        }

        void ReaperTickOne(LockTarget& tgt, std::uint64_t now_ms) {
            const auto owner = ReadOwner(tgt.addr);
            const auto state = ReadState(tgt.addr);

            if (state == 0) {
                tgt.prev_owner      = 0;
                tgt.prev_state      = 0;
                tgt.stable_since_ms = 0;
                return;
            }

            if (owner != tgt.prev_owner || state != tgt.prev_state) {
                tgt.prev_owner      = owner;
                tgt.prev_state      = state;
                tgt.stable_since_ms = now_ms;
                return;
            }

            const auto held_ms = now_ms - tgt.stable_since_ms;
            if (held_ms < kStaleThresholdMs) {
                return;
            }

            if (IsThreadAlive(owner)) {
                Stats::OnLiveSkip(tgt.name);
                return;
            }

            const bool won = ForceRelease(tgt, state);
            if (won) {
                Stats::OnReaped(tgt.name);
                logs::warn(
                    "[REAPER] force-released {} (was held by phantom "
                    "TID {} with state={} for ~{}ms; CAS state->0 "
                    "succeeded). With Approach G active this is "
                    "unexpected; please report.",
                    tgt.name, owner, state, held_ms);
            } else {
                Stats::OnRace(tgt.name);
                logs::trace(
                    "[REAPER] {} CAS lost to engine modification "
                    "(observed owner={} state={}). Backing off.",
                    tgt.name, owner, state);
            }
            tgt.prev_owner      = 0;
            tgt.prev_state      = 0;
            tgt.stable_since_ms = 0;
        }

        void ReaperBody() {
            logs::info("[REAPER] watchdog thread starting "
                       "(check_interval={}ms, stale_threshold={}ms).",
                       kCheckInterval.count(), kStaleThresholdMs);
            while (!g_reaper_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(kCheckInterval);
                if (g_reaper_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                const auto now_ms = ::GetTickCount64();
                for (auto& tgt : g_targets) {
                    ReaperTickOne(tgt, now_ms);
                }
            }
            logs::info("[REAPER] watchdog thread exiting.");
        }

        // ---- Call-site table (CallSites.inc convention) -------------------
        enum CallSiteKind : std::uint8_t {
            kCall = 0,
            kJmp  = 1,
        };

        struct CallSite {
            std::uint64_t  id;
            std::ptrdiff_t offset;
            CallSiteKind   kind;
        };

#include "CallSites_G.inc"

        template <std::size_t N>
        std::pair<std::size_t, std::uintptr_t> InstallSites(
            const CallSite (&sites)[N],
            std::uintptr_t hook_addr,
            std::string_view label)
        {
            auto& tramp = SKSE::GetTrampoline();
            std::size_t installed = 0;
            std::uintptr_t orig = 0;

            for (const auto& site : sites) {
                try {
                    const REL::Relocation<std::uintptr_t> reloc{
                        REL::ID(site.id), site.offset
                    };
                    std::uintptr_t prev_target = 0;
                    if (site.kind == kCall) {
                        prev_target = tramp.write_call<5>(
                            reloc.address(), hook_addr);
                    } else {
                        prev_target = tramp.write_branch<5>(
                            reloc.address(), hook_addr);
                    }
                    if (orig == 0) {
                        orig = prev_target;
                    }
                    ++installed;
                }
                catch (const std::exception& e) {
                    logs::warn(
                        "[{}] failed to patch site id {} +0x{:x} ({}): {}",
                        label, site.id, site.offset,
                        site.kind == kCall ? "CALL" : "JMP",
                        e.what());
                }
            }

            return { installed, orig };
        }

        bool InstallApproachG() {
            // 2 unique destinations (Hook_19369, Hook_40706) share one
            // 14-byte trampoline stub each (CommonLibSSE-NG dedupes by
            // destination). 28 bytes minimum; 256 keeps headroom for
            // any internal allocator overhead.
            SKSE::AllocTrampoline(256);

            constexpr std::size_t exp_19369 =
                sizeof(kCallSites_19369) / sizeof(kCallSites_19369[0]);
            constexpr std::size_t exp_40706 =
                sizeof(kCallSites_40706) / sizeof(kCallSites_40706[0]);

            const auto [n_19369, orig_19369] = InstallSites(
                kCallSites_19369,
                reinterpret_cast<std::uintptr_t>(&Hook_19369),
                "id_19369");
            const auto [n_40706, orig_40706] = InstallSites(
                kCallSites_40706,
                reinterpret_cast<std::uintptr_t>(&Hook_40706),
                "id_40706");

            if (orig_19369 == 0 || orig_40706 == 0) {
                logs::critical(
                    "Approach G install produced no usable trampoline "
                    "targets (id_19369={}/{}, id_40706={}/{}).",
                    n_19369, exp_19369, n_40706, exp_40706);
                return false;
            }

            g_orig_19369 = reinterpret_cast<Fn19369>(orig_19369);
            g_orig_40706 = reinterpret_cast<Fn40706>(orig_40706);

            logs::info(
                "Approach G call-site hooks installed: "
                "id_19369 -> {}/{} sites, orig=0x{:x}; "
                "id_40706 -> {}/{} sites, orig=0x{:x}.",
                n_19369, exp_19369, orig_19369,
                n_40706, exp_40706, orig_40706);

            if (n_19369 != exp_19369 || n_40706 != exp_40706) {
                logs::warn(
                    "Some call sites failed to patch. Coverage is "
                    "incomplete; an unhooked entry to id_19369 or "
                    "id_40706 could still let AB-BA form. Check "
                    "warnings above.");
            }
            return true;
        }

        bool InstallApproachF() {
            const auto module_base = REL::Module::get().base();
            g_targets[0].addr = module_base + kLockA_RVA;
            g_targets[1].addr = module_base + kLockB_RVA;

            logs::info(
                "Lock observation: LockA @ 0x{:x} (RVA 0x{:x}), "
                "LockB @ 0x{:x} (RVA 0x{:x}).",
                g_targets[0].addr, kLockA_RVA,
                g_targets[1].addr, kLockB_RVA);

            if (g_reaper_started.exchange(true, std::memory_order_acq_rel)) {
                return true;
            }

            g_reaper_stop.store(false, std::memory_order_relaxed);
            try {
                g_reaper_thread = std::thread(ReaperBody);
                g_reaper_thread.detach();
            }
            catch (const std::exception& e) {
                logs::critical("Failed to start reaper thread: {}", e.what());
                g_reaper_started.store(false, std::memory_order_release);
                return false;
            }

            logs::info(
                "Approach F (stale-owner reaper) active as a safety "
                "net under Approach G. Reaped events should remain at "
                "zero in normal operation.");
            return true;
        }

    } // namespace

    bool Install() {
        // Compute LockA / LockB absolute addresses up front. Approach G's
        // narrowed Hook_40706 needs g_lockB_addr_for_narrow to be set
        // BEFORE we patch any call sites; otherwise the first
        // invocation that races us in would see 0 and pass through
        // unconditionally. Approach F also reads g_targets[*].addr.
        const auto module_base = REL::Module::get().base();
        g_targets[0].addr           = module_base + kLockA_RVA;
        g_targets[1].addr           = module_base + kLockB_RVA;
        g_lockB_addr_for_narrow     = g_targets[1].addr;

        if (!InstallApproachG()) {
            logs::critical(
                "Approach G install failed. The plugin will run with "
                "Approach F (the v0.11 reaper) only, which contains "
                "phantom-owner failures but does NOT prevent the "
                "live AB-BA freeze. Check warnings above.");
            // Still install F as a degraded fallback.
            InstallApproachF();
            return false;
        }
        if (!InstallApproachF()) {
            logs::warn(
                "Approach F (reaper) install failed; Approach G is "
                "still active so the structural fix should hold, but "
                "the safety net is missing.");
            // G alone is still a fix, so we don't fail Install().
        }

        logs::info(
            "WorkerSpinLockFix v0.13 fully armed. Structural fix: "
            "shared std::recursive_mutex serialises every entry to "
            "id 19369, plus id 40706 invocations whose this+0x150 "
            "resolves to the global LockB at 0x{:x}. Per-object "
            "id 40706 instances pass through. Safety net: 250 ms "
            "reaper watching LockA / LockB for phantom owners.",
            g_lockB_addr_for_narrow);
        return true;
    }

}
