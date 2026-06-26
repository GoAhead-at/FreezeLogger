#pragma once

#include <cstdint>

namespace WorkerSpinLockFix::SkyrimAnchors {

    // Runtime-resolved engine anchors for the WaitForJobTask / Singleton-B
    // job-pool chain. Ported from the FreezeLogger diagnostics plugin, where
    // the same scan drives the Site-B freeze probe.
    //
    // Address Library IDs are NOT identity-stable for these targets (the id
    // that is WaitForJobTask on SE 1.5.97 is a different function on AE, and
    // Singleton-B has no AE id at all), so instead of per-version RVA tables
    // we locate WaitForJobTask by a byte signature in the loaded module's
    // .text at startup and *derive* the Singleton-B slot from its rip-relative
    // load. This is version-independent: SE 1.5.97, every AE point release,
    // and VR all resolve from the same 15-byte body signature with no Address
    // Library dependency. If the signature is not found, Available() stays
    // false and the JobWaitBreaker stays idle instead of dereferencing a
    // stale RVA.
    //
    //   WaitForJobTask body (the bytes we anchor on, after the version-
    //   variable `mov rax,[rip+disp]` prologue):
    //     mov  r8d, ecx          44 8B C1      ; r8d = job index (arg0)
    //     mov  rcx, [rax + 8]    48 8B 48 08   ; rcx = sub-array  = [instance+8]
    //     mov  rcx, [rcx+r8*8]   4A 8B 0C C1   ; rcx = element[idx]
    //     test rcx, rcx          48 85 C9      ; if element == null ...
    //     je   short             74            ;   ... skip the wait (no job)
    //
    // The chain that the breaker walks at runtime:
    //   instance = *(singletonBSlot)
    //   subArray = *(instance + 8)
    //   element  = *(subArray + idx*8)
    // When main parks in WaitForJobTask, `element` was non-null (otherwise the
    // engine would have taken the `je` and not waited). The lost-wakeup freeze
    // is exactly the case where `subArray` (and/or `element`) is torn down to
    // null AFTER main is asleep, with no thread left to signal it.
    struct Anchors {
        bool           resolved          = false;

        std::uintptr_t moduleBase        = 0;
        std::uintptr_t moduleSize        = 0;
        const wchar_t* moduleName        = L"";

        // Absolute VAs in the running process.
        std::uintptr_t waitForJobTask    = 0;  // function entry (to wrap)
        std::uintptr_t singletonBSlot    = 0;  // address of the global ptr slot

        // RVAs (module-relative) for logging.
        std::uintptr_t waitForJobTaskRVA = 0;
        std::uintptr_t singletonBSlotRVA = 0;

        // ----- Site-A (id 34554 worker-ack wait helper) -----------------
        // The SECOND Main::Update infinite-wait site (case-study 29). It
        // is a separate, smaller function from WaitForJobTask: it loads a
        // singleton (Singleton-A) from a rip-relative slot, and -- when
        // that singleton's pending flag [+0x6c]==1 -- waits INFINITE on the
        // worker-ack event [+0x60] for a worker to signal completion. The
        // freeze is the case where the worker consumes the wake but never
        // signals the ack, so main sleeps forever. The SiteABreaker wraps
        // this function. Resolved by an independent body signature scan, so
        // a missing/mismatched Site-A leaves the SiteABreaker idle without
        // affecting the WaitForJobTask (Site-B) resolution above.
        bool           siteAResolved     = false;
        std::uintptr_t siteALockFn       = 0;  // id 34554 entry (to wrap)
        std::uintptr_t singletonASlot    = 0;  // address of the global ptr slot
        std::uintptr_t siteALockFnRVA    = 0;
        std::uintptr_t singletonASlotRVA = 0;

        // ----- Render-side Site-A join (SE id 34557) --------------------
        // The render/worker thread's analog of main's Site-A. The engine's
        // parallel cloth dispatch (driven by Faster HDT-SMP) runs a worker
        // loop (id 34567) that, per work item, calls a per-task join
        // function (id 34557) which waits INFINITE on each dispatched
        // sub-task before signalling Singleton-A's worker-ack [+0x60]. When
        // a sub-task's worker is stuck/gone, the render thread parks forever
        // inside id 34557 and never acks -- the render counterpart of the
        // main-side worker-ack deadlock (case-study 29 §6; first captured in
        // freeze_2026-06-24_053440). Unlike id 34554, id 34557 receives its
        // Singleton-A in rcx (no rip-relative slot), and `mov rbp,rcx` at
        // entry, so the SiteARenderBreaker captures the singleton from the
        // first argument at wrap entry rather than from a derived slot.
        // Anchored on the byte-unique entry prologue
        //   push rbx; push rsi; push rdi; sub rsp,0x30; mov byte [rcx+0x71],1
        // (40 53 56 57 48 83 EC 30 C6 41 71 01).
        bool           renderTaskResolved = false;
        std::uintptr_t renderTaskFn       = 0;  // id 34557 entry (to wrap)
        std::uintptr_t renderTaskFnRVA    = 0;

        // ----- BSSpinLock spin-retry site(s) (LeakedSpinLockBreaker) ----
        // The address(es) a thread parks/loops on while acquiring a
        // BSSpinLock (SkyrimSE id 12210, BSSpinLock::Acquire). The lock
        // body's spin loop is `inc ebx; xor ecx,ecx; call <yield>; xor
        // eax,eax; lock cmpxchg [rdi+4], r14d`, and the `xor eax,eax`
        // right after the yield call is the return address a contending
        // thread shows on its stack. The call displacement is the only
        // version-variable field, so the signature wildcards bytes 6..9
        // and resolves on SE/AE/VR alike. The LeakedSpinLockBreaker scans
        // suspended threads for these return addresses to find a thread
        // contending a BSSpinLock, then follows the lock's [+0] owner.
        // Several spin-retry sites can exist (the lock acquire is inlined
        // in a handful of places), so we collect up to kMaxSpinRets of
        // them. If none is found, AvailableSpinRetry() stays false and
        // the breaker stays idle.
        static constexpr std::size_t kMaxSpinRets = 8;
        bool           spinResolved = false;
        std::uint32_t  spinRetCount = 0;
        std::uintptr_t spinRets[kMaxSpinRets] = {};      // absolute VAs
        std::uintptr_t spinRetsRVA[kMaxSpinRets] = {};   // module-relative
    };

    // Resolve once (idempotent). Safe to call from SKSEPlugin_Load onward.
    void Init();

    [[nodiscard]] const Anchors& Get() noexcept;

    // True iff the signature scan found WaitForJobTask AND derived a
    // non-null Singleton-B slot. The JobWaitBreaker gates on this.
    [[nodiscard]] bool Available() noexcept;

    // True iff the Site-A signature scan found id 34554 AND derived a
    // non-null Singleton-A slot. The SiteABreaker gates on this.
    [[nodiscard]] bool AvailableSiteA() noexcept;

    // True iff the render-side join function (id 34557) was resolved. The
    // SiteARenderBreaker gates on this.
    [[nodiscard]] bool AvailableSiteARender() noexcept;

    // True iff at least one BSSpinLock spin-retry site was resolved. The
    // LeakedSpinLockBreaker gates on this.
    [[nodiscard]] bool AvailableSpinRetry() noexcept;

    // Human-readable one-liner for logs (Site-B / WaitForJobTask).
    [[nodiscard]] const char* DiagnosticString();

    // Human-readable one-liner for logs (Site-A / id 34554).
    [[nodiscard]] const char* DiagnosticStringSiteA();

}
