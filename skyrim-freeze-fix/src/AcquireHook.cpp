#include "PCH.h"
#include "AcquireHook.h"
#include "Breaker.h"
#include "Stats.h"
#include "WaitGraph.h"

namespace WorkerSpinLockFix::AcquireHook {

    namespace {

        // RVAs from static analysis (case-study/06-root-cause.md and
        // confirmed live across 9 pre-WSLF freeze captures). The two
        // engine BSSpinLocks that race in vanilla Skyrim 1.5.97 worker-
        // pool dispatch.
        constexpr std::uintptr_t kLockA_RVA = 0x2eff8e0;
        constexpr std::uintptr_t kLockB_RVA = 0x2f3b8e8;

        SafetyHookInline   g_acquire_hook{};
        std::uintptr_t     g_spin_retry_addr{ 0 };
        std::atomic<bool>  g_installed{ false };

        // Surgical filter targets. Resolved at Install() (or earlier via
        // ResolveLockPointers). Read from the hot path so they are kept
        // in plain pointers, not atomics: they are written exactly once
        // before the hook becomes active and never modified afterwards.
        WaitGraph::Lock*   g_lockA{ nullptr };
        WaitGraph::Lock*   g_lockB{ nullptr };

        // Optional test-mode targets. Default nullptr (always compares
        // unequal to any real BSSpinLock pointer, so the filter behaves
        // exactly like the production filter). Populated by AddTestLocks
        // when TestMode is enabled and runs its synthetic AB-BA. Reading
        // these in the hot path is safe under one-shot write + cooperative
        // ordering: they are written once at TestMode init before the
        // test threads start, and never modified afterwards. We tolerate
        // a torn read (a moment where the second pointer is visible but
        // the first is not) because the worst case is a single missed
        // detection on the very first acquire of one test lock.
        WaitGraph::Lock*   g_test_lockA{ nullptr };
        WaitGraph::Lock*   g_test_lockB{ nullptr };

        // The hot path.
        //
        // SURGICAL FILTER: 99.999% of BSSpinLock::Acquire calls in the
        // engine touch locks that are not LockA or LockB. Those calls
        // pay only one comparison against a constant + one branch + one
        // tail-call to the trampoline (~2 ns at 3 GHz). The slow path
        // and wait-graph machinery only run for LockA and LockB, which
        // are contended at most a handful of times per second.
        //
        // For the two surgical locks themselves these rules apply:
        //
        //   - state (+0x4) IS authoritative for "held". 0 = free, 1 = held.
        //     owner (+0x0) is NOT (engine does not always clear it).
        //   - No heap allocation on this path. Allocating here puts the
        //     heap CRITICAL_SECTION on the BSSpinLock lock-order graph
        //     and deadlocks against legitimate engine paths.
        //   - No std::mutex on this path. SRWLocks under std::mutex
        //     create the same lock-order edge.
        //   - No spdlog calls on this path (logging is gated to actual
        //     cycle observations only, which run rarely).
        //
        // CRITICAL: we MUST use safetyhook::InlineHook::unsafe_call rather
        // than .call<>(). The latter takes a std::recursive_mutex (m_mutex)
        // for thread-safe install/uninstall coordination. With ~300 engine
        // threads each routing every BSSpinLock::Acquire through this
        // hook, that mutex would serialise every acquire across all
        // threads and create a global SRWLock-vs-BSSpinLock lock-order
        // edge sufficient to freeze early engine initialisation.
        // unsafe_call skips the mutex and just tail-calls through the
        // trampoline. We never uninstall the hook at runtime, so we
        // cannot race with installation/destruction either.
        void __fastcall HookedAcquire(WaitGraph::Lock* self) {
            // SURGICAL FILTER. Up to four pointer compares against
            // constants. Branch predictor sees one outcome ~always
            // (taken: bypass). g_test_lockA / g_test_lockB are normally
            // both nullptr, so the two extra compares cost ~zero in
            // production; they only ever become non-null when TestMode
            // is explicitly enabled in the TOML.
            if (self != g_lockA && self != g_lockB &&
                self != g_test_lockA && self != g_test_lockB) {
                g_acquire_hook.unsafe_call<void>(self);
                return;
            }

            // Fast path A: lock is free.
            if (self->state == 0) {
                g_acquire_hook.unsafe_call<void>(self);
                return;
            }

            const DWORD me = ::GetCurrentThreadId();

            // Fast path B: recursive acquire (we already own it).
            if (self->owner == me) {
                g_acquire_hook.unsafe_call<void>(self);
                return;
            }

            // Slow path: contended LockA or LockB.
            Stats::OnAcquireSlow();

            std::array<WaitGraph::CycleParticipant,
                       WaitGraph::kMaxHops> chain;

            WaitGraph::EnterSlow(me, self);
            const int chain_len = WaitGraph::WouldFormCycle(
                me, self, chain.data(),
                static_cast<int>(chain.size()));
            if (chain_len >= 2) {
                Breaker::OnCycleDetected(me, self, chain.data(), chain_len);
            }

            g_acquire_hook.unsafe_call<void>(self);

            WaitGraph::ExitSlow(me);
        }

    } // namespace

    WaitGraph::Lock* LockA() noexcept { return g_lockA; }
    WaitGraph::Lock* LockB() noexcept { return g_lockB; }

    void AddTestLocks(WaitGraph::Lock* a, WaitGraph::Lock* b) noexcept {
        g_test_lockA = a;
        g_test_lockB = b;
    }

    void ResolveLockPointers() noexcept {
        if (g_lockA != nullptr && g_lockB != nullptr) {
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(
            ::GetModuleHandleW(L"SkyrimSE.exe"));
        if (base == 0) {
            logs::critical(
                "[AcquireHook] could not resolve SkyrimSE.exe module base; "
                "surgical filter cannot be initialised. Hook will be inert "
                "(every Acquire will fast-path).");
            return;
        }
        g_lockA = reinterpret_cast<WaitGraph::Lock*>(base + kLockA_RVA);
        g_lockB = reinterpret_cast<WaitGraph::Lock*>(base + kLockB_RVA);
    }

    std::uintptr_t SpinRetryAddress() noexcept {
        return g_spin_retry_addr;
    }

    std::uintptr_t ResolveSpinRetryAddress() noexcept {
        if (g_spin_retry_addr != 0) {
            return g_spin_retry_addr;
        }
        try {
            const REL::Relocation<std::uintptr_t> acquire{ REL::ID(12210) };
            g_spin_retry_addr = acquire.address() + 0x8a;
            return g_spin_retry_addr;
        } catch (const std::exception& e) {
            logs::critical(
                "[AcquireHook] failed to resolve BSSpinLock::Acquire "
                "(id 12210): {}", e.what());
            return 0;
        }
    }

    bool Install() {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        ResolveLockPointers();
        if (g_lockA == nullptr || g_lockB == nullptr) {
            logs::critical(
                "[AcquireHook] LockA/LockB pointers not resolved; aborting "
                "hook install.");
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        try {
            const REL::Relocation<std::uintptr_t> acquire{ REL::ID(12210) };
            const auto target = acquire.address();
            g_spin_retry_addr = target + 0x8a;

            auto inline_hook = safetyhook::create_inline(
                reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(&HookedAcquire));

            if (!inline_hook) {
                logs::critical(
                    "[AcquireHook] safetyhook::create_inline FAILED at "
                    "0x{:x}; entry-point hook is not active.",
                    target);
                g_installed.store(false, std::memory_order_release);
                return false;
            }

            g_acquire_hook = std::move(inline_hook);

            logs::info(
                "[AcquireHook] surgical entry-point hook installed on "
                "BSSpinLock::Acquire (id 12210, addr=0x{:x}, "
                "spin_retry=0x{:x}).",
                target,
                g_spin_retry_addr);
            logs::info(
                "[AcquireHook] surgical filter targets: LockA=0x{:x} "
                "(RVA 0x{:x}), LockB=0x{:x} (RVA 0x{:x}). All other "
                "BSSpinLocks fast-path bypass our slow path.",
                reinterpret_cast<std::uintptr_t>(g_lockA), kLockA_RVA,
                reinterpret_cast<std::uintptr_t>(g_lockB), kLockB_RVA);
            return true;
        } catch (const std::exception& e) {
            logs::critical(
                "[AcquireHook] failed to resolve/hook BSSpinLock::Acquire: "
                "{}", e.what());
            g_installed.store(false, std::memory_order_release);
            return false;
        }
    }

}
