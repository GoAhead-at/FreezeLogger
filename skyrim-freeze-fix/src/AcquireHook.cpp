#include "PCH.h"
#include "AcquireHook.h"
#include "Breaker.h"
#include "Stats.h"
#include "WaitGraph.h"

namespace WorkerSpinLockFix::AcquireHook {

    namespace {

        // RVAs from static analysis (case-study/06-root-cause.md and
        // confirmed live across 9 pre-WSLF freeze captures). The two
        // engine BSSpinLocks that race in vanilla Skyrim worker-pool
        // dispatch.
        //
        // AE 1.6.x RVAs were re-derived from the unpacked AE binary
        // (analysis/ae_recursive_lockers.py pins LockA as the lock held
        // by the recursive acquirer id 19796; analysis/ae_find_lockb.py
        // pins LockB as the lock guarding the Add/Remove temp-change
        // twins id 41343/41344). LockA/LockB are static globals, so the
        // module-relative RVA is what we need.
        //   LockA: SE 0x2eff8e0 -> AE 0x31376d8
        //   LockB: SE 0x2f3b8e8 -> AE 0x319c2a8
        inline std::uintptr_t LockA_RVA() noexcept {
            return REL::Module::IsAE() ? 0x31376d8 : 0x2eff8e0;
        }
        inline std::uintptr_t LockB_RVA() noexcept {
            return REL::Module::IsAE() ? 0x319c2a8 : 0x2f3b8e8;
        }

        SafetyHookMid      g_spin_retry_hook{};
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

        // The detection path -- a COLD path by construction.
        //
        // This is a safetyhook MID-hook planted at BSSpinLock::Acquire
        // +0x8a (id 12210), the instruction immediately after the engine's
        // backoff-yield call inside the OUTER spin loop and immediately
        // before the retry CAS. The disassembly that fixes this offset is
        // in analysis/dump_acquire.py; the salient facts:
        //
        //   - The uncontended fast path (lock free) and the recursive
        //     re-entry path both return long before +0x8a. The inner
        //     `_mm_pause` spin (bounded by the engine's iLimit) also exits
        //     before +0x8a. ONLY a thread that has exhausted the inner
        //     spin AND returned from at least one yield reaches here -- the
        //     genuinely-stuck population. So unlike the old entry-point
        //     inline hook, the ~99.99% of acquires that never seriously
        //     contend pay ZERO overhead: they run entirely native.
        //   - At +0x8a the lock pointer `self` is in RDI (set at function
        //     entry, callee-saved, never reassigned in the body) and the
        //     owning thread id is in EBP. We read `self` from ctx.rdi and
        //     take `me` from GetCurrentThreadId() (identical to EBP, but
        //     robust against any register-analysis error since the callback
        //     runs on the very thread that is spinning).
        //   - We do NOT call any trampoline. A mid-hook is observational:
        //     after this callback returns, safetyhook re-executes the
        //     displaced instructions and the engine's own spin loop
        //     continues. The breaker unsticks a confirmed cycle by CASing
        //     the lock's state 1->0, which lets the engine's own retry CAS
        //     at +0x8c succeed.
        //
        // Lock-order safety (unchanged from the old design): at +0x8a the
        // thread holds NO BSSpinLock -- it is trying to acquire one. So the
        // SRWLock/std::mutex and the sleep taken inside Breaker::
        // OnCycleDetected (only when chain_len >= 2, i.e. rarely) cannot
        // introduce a (BSSpinLock -> SRWLock) lock-order edge. EnterSlow and
        // WouldFormCycle are heap-allocation-free.
        void OnSpinRetry(SafetyHookContext& ctx) {
            auto* self = reinterpret_cast<WaitGraph::Lock*>(ctx.rdi);

            // SURGICAL FILTER. Up to four pointer compares. g_test_lockA /
            // g_test_lockB are normally both nullptr, so they never match a
            // real BSSpinLock; they only become non-null when TestMode is
            // explicitly enabled in the TOML.
            if (self != g_lockA && self != g_lockB &&
                self != g_test_lockA && self != g_test_lockB) {
                return;
            }

            const DWORD me = ::GetCurrentThreadId();

            // Publish / refresh our wait edge. EnterSlow returns true only
            // on the first publish of this target for this thread, so the
            // slow-path counter measures distinct stuck episodes rather
            // than backoff iterations.
            if (WaitGraph::EnterSlow(me, self)) {
                Stats::OnAcquireSlow();
            }

            std::array<WaitGraph::CycleParticipant,
                       WaitGraph::kMaxHops> chain;

            const int chain_len = WaitGraph::WouldFormCycle(
                me, self, chain.data(),
                static_cast<int>(chain.size()));
            if (chain_len >= 2) {
                Breaker::OnCycleDetected(me, self, chain.data(), chain_len);
            }
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
        g_lockA = reinterpret_cast<WaitGraph::Lock*>(base + LockA_RVA());
        g_lockB = reinterpret_cast<WaitGraph::Lock*>(base + LockB_RVA());
    }

    std::uintptr_t SpinRetryAddress() noexcept {
        return g_spin_retry_addr;
    }

    std::uintptr_t ResolveSpinRetryAddress() noexcept {
        if (g_spin_retry_addr != 0) {
            return g_spin_retry_addr;
        }
        try {
            // BSSpinLock::Acquire: SE id 12210 -> AE id 13663. The
            // +0x8a spin-retry offset and the self-in-RDI / tid-in-EBP
            // register contract are identical on both runtimes (verified
            // by disassembly: analysis/ae_dump.py id 13663).
            const REL::Relocation<std::uintptr_t> acquire{
                REL::RelocationID(12210, 13663) };
            g_spin_retry_addr = acquire.address() + 0x8a;
            return g_spin_retry_addr;
        } catch (const std::exception& e) {
            logs::critical(
                "[AcquireHook] failed to resolve BSSpinLock::Acquire "
                "(SE id 12210 / AE id 13663): {}", e.what());
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

        const auto retry_addr = ResolveSpinRetryAddress();
        if (retry_addr == 0) {
            logs::critical(
                "[AcquireHook] spin-retry address (id 12210 + 0x8a) not "
                "resolved; aborting hook install.");
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        auto mid_hook = safetyhook::create_mid(
            reinterpret_cast<void*>(retry_addr), &OnSpinRetry);

        if (!mid_hook) {
            logs::critical(
                "[AcquireHook] safetyhook::create_mid FAILED at 0x{:x}; "
                "spin-retry hook is not active.",
                retry_addr);
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        g_spin_retry_hook = std::move(mid_hook);

        logs::info(
            "[AcquireHook] spin-retry mid-hook installed at "
            "BSSpinLock::Acquire+0x8a (id 12210, addr=0x{:x}). Uncontended "
            "and recursive acquires run fully native; only a watched lock's "
            "outer backoff loop enters cycle detection.",
            retry_addr);
        logs::info(
            "[AcquireHook] surgical filter targets: LockA=0x{:x} "
            "(RVA 0x{:x}), LockB=0x{:x} (RVA 0x{:x}). All other "
            "BSSpinLocks are invisible to the detector.",
            reinterpret_cast<std::uintptr_t>(g_lockA), LockA_RVA(),
            reinterpret_cast<std::uintptr_t>(g_lockB), LockB_RVA());
        return true;
    }

}
