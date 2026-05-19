#include "PCH.h"
#include "Hooks.h"
#include "Config.h"
#include "Stats.h"

// =============================================================================
// WorkerSpinLockFix v0.11 - Strategy F: stale-owner reaper.
//
// Why this replaces v0.10 (Strategy A applied at the call-site level):
//   v0.10 hooked every direct CALL/JMP site of BSSpinLock::Acquire
//   (id 12210, 1529 sites) and BSSpinLock::Release (id 66983,
//   725 sites) and forced LockA-before-LockB ordering. It worked
//   for Acquire but COMPLETELY MISSED the Release side, because
//   id 66983 is a 28-byte function that the compiler inlined at
//   most call sites. The 60-second stats from the v0.10 freeze
//   showed:
//
//       LockA acq=0 rel=0
//       LockB acq=3492 rel=0     <-- 3492 acquires, 0 releases
//       extra_LockA taken=1 released=0 (outstanding=1)
//
//   We auto-acquired LockA on a worker, the worker eventually
//   exited (after releasing LockB through inlined code we never
//   saw), and LockA stayed held by a phantom TID. Main thread
//   later tried to take LockA, deadlocked, freeze.
//
// What v0.11 does instead:
//   Run a low-overhead background thread that, every ~250ms,
//   reads the state field of LockA and LockB. If a lock has been
//   held by the same TID continuously for longer than a staleness
//   threshold (default 2s) AND that TID is no longer alive in this
//   process (OpenThread fails or GetExitCodeThread returns a
//   non-STILL_ACTIVE code), the watchdog force-releases it.
//
//   The force-release is a CAS on the state field: we attempt to
//   change it from the value we just observed back to 0. If a real
//   engine thread modified the lock between our read and the CAS,
//   the CAS fails and we back off (counted as a "race"). Owner
//   field is left untouched - id 66983's analysis showed that
//   normal engine releases never clear it either.
//
// Important properties:
//   * Zero hooks. Nothing in our code is in the lock fast path.
//     If we crash, the engine still owns the locks and behaves
//     exactly as if the plugin were not installed.
//   * Conservative. We only act on locks held by a CONFIRMED
//     dead TID. A live thread sitting on a lock for 10s never
//     gets reaped, even if the game looks frozen to the user.
//   * Targets ONLY the two locks we have evidence for (LockA at
//     RVA 0x2eff8e0 and LockB at RVA 0x2f3b8e8). We do not scan
//     for other BSSpinLocks. If a different lock starts leaking,
//     we will need to add it explicitly.
//
// What this does NOT solve:
//   * A genuine AB-BA deadlock where both holder threads are
//     still alive and just spinning forever. The reaper will see
//     "alive" and skip. v0.11 is a strict superset of "do nothing"
//     for that scenario.
//   * Latency: the watchdog runs every 250ms and waits 2s before
//     reaping. Worst-case freeze before recovery is ~2-2.5s.
// =============================================================================

namespace WorkerSpinLockFix::Hooks {

    namespace {

        // ---- Lock layout ---------------------------------------------------
        constexpr std::uintptr_t kLockA_RVA = 0x2eff8e0;
        constexpr std::uintptr_t kLockB_RVA = 0x2f3b8e8;

        struct LockTarget {
            std::uintptr_t addr      = 0;
            const char*    name      = nullptr;

            // Tracking, only ever read/written by the watchdog thread.
            std::uint32_t  prev_owner       = 0;
            std::uint32_t  prev_state       = 0;
            std::uint64_t  stable_since_ms  = 0;
        };

        LockTarget g_targets[2] = {
            { 0, "LockA" },
            { 0, "LockB" },
        };

        std::atomic<bool> g_stop{ false };
        std::atomic<bool> g_started{ false };
        std::thread       g_thread;

        // Watchdog timing. Conservative defaults to avoid false positives.
        constexpr auto      kCheckInterval     = std::chrono::milliseconds(250);
        constexpr std::uint64_t kStaleThresholdMs = 2000;

        // ---- Lock memory access -------------------------------------------
        inline std::uint32_t ReadOwner(std::uintptr_t addr) {
            return *reinterpret_cast<const volatile std::uint32_t*>(addr);
        }
        inline std::uint32_t ReadState(std::uintptr_t addr) {
            return *reinterpret_cast<const volatile std::uint32_t*>(addr + 4);
        }

        // ---- Thread liveness ----------------------------------------------
        bool IsThreadAlive(std::uint32_t tid) {
            if (tid == 0) {
                return false;
            }
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
                                    FALSE, tid);
            if (h == nullptr) {
                // Most common cause: TID does not exist (or belongs to a
                // different process). Either way, not alive in our process.
                return false;
            }
            DWORD code = 0;
            BOOL  ok   = ::GetExitCodeThread(h, &code);
            ::CloseHandle(h);
            if (!ok) {
                return false;
            }
            return code == STILL_ACTIVE;
        }

        // ---- Force-release ------------------------------------------------
        //
        // We zero the state field via CAS, observed_state -> 0. We do not
        // touch the owner field: the engine's normal release path doesn't
        // clear it either, and we've confirmed by static analysis that
        // ownership decisions are made off the state field's transition,
        // not off the owner's value.
        //
        // Returns true if we won the CAS, false if a concurrent engine
        // thread modified the lock first.
        bool ForceRelease(LockTarget& tgt, std::uint32_t observed_state) {
            auto* state_ptr =
                reinterpret_cast<volatile LONG*>(tgt.addr + 4);
            const LONG prev = ::InterlockedCompareExchange(
                state_ptr,
                /*exchange*/ 0,
                /*comperand*/ static_cast<LONG>(observed_state));
            return prev == static_cast<LONG>(observed_state);
        }

        // ---- Per-target tick ----------------------------------------------
        void TickOne(LockTarget& tgt, std::uint64_t now_ms) {
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

            // Same (owner, state) for >=kStaleThresholdMs and the owner
            // is no longer a thread in this process. Force-release.
            const bool won = ForceRelease(tgt, state);
            if (won) {
                Stats::OnReaped(tgt.name);
                logs::warn(
                    "[REAPER] force-released {} (was held by phantom "
                    "TID {} with state={} for ~{}ms; CAS state->0 "
                    "succeeded).",
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

        // ---- Watchdog body -------------------------------------------------
        void WatchdogBody() {
            logs::info("[REAPER] watchdog thread starting "
                       "(check_interval={}ms, stale_threshold={}ms).",
                       kCheckInterval.count(), kStaleThresholdMs);

            while (!g_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(kCheckInterval);
                if (g_stop.load(std::memory_order_relaxed)) {
                    break;
                }
                const auto now_ms = ::GetTickCount64();
                for (auto& tgt : g_targets) {
                    TickOne(tgt, now_ms);
                }
            }

            logs::info("[REAPER] watchdog thread exiting.");
        }

    } // namespace

    bool Install() {
        const auto module_base = REL::Module::get().base();
        g_targets[0].addr = module_base + kLockA_RVA;
        g_targets[1].addr = module_base + kLockB_RVA;

        logs::info(
            "Lock observation: LockA @ 0x{:x} (RVA 0x{:x}), "
            "LockB @ 0x{:x} (RVA 0x{:x}).",
            g_targets[0].addr, kLockA_RVA,
            g_targets[1].addr, kLockB_RVA);

        if (g_started.exchange(true, std::memory_order_acq_rel)) {
            logs::warn("Hooks::Install called twice; ignoring.");
            return true;
        }

        g_stop.store(false, std::memory_order_relaxed);
        try {
            g_thread = std::thread(WatchdogBody);
            g_thread.detach();
        }
        catch (const std::exception& e) {
            logs::critical("Failed to start reaper thread: {}", e.what());
            g_started.store(false, std::memory_order_release);
            return false;
        }

        logs::info(
            "Strategy F active (v0.11, stale-owner reaper). The plugin "
            "installs NO hooks and is not in any lock fast path. A "
            "background thread will force-release LockA / LockB if "
            "either is held continuously for >2s by a TID that no "
            "longer exists in the process.");
        return true;
    }

}
