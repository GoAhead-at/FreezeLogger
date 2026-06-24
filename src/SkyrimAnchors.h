#pragma once

#include <cstdint>

namespace FreezeLogger::SkyrimAnchors {

    // Runtime-resolved engine anchors for the Site-B / WaitForJobTask probe.
    //
    // Historically these were hard-coded SE 1.5.97 RVAs (Singleton-B at
    // +0x2f26a70, WaitForJobTask at +0xc38130). That pinned the whole deep
    // probe to one runtime AND carried a latent bug: the engine actually
    // loads Singleton-B from +0x2f26670, not +0x2f26a70 (an old +0x400
    // arithmetic slip — see analysis/disasm_waitforjob.py).
    //
    // Instead of per-version RVA tables (Address Library IDs are NOT
    // identity-stable for these — id 68167 is WaitForJobTask on SE but a
    // different function on AE, and Singleton-B has no AE id at all), we
    // locate WaitForJobTask by a byte signature in the loaded module's
    // .text at startup and *derive* Singleton-B from its rip-relative load.
    // This is version-independent: SE 1.5.97, every AE point release, and
    // VR all resolve from the same 15-byte body signature with no Address
    // Library dependency. If the signature is not found (an engine whose
    // dispatcher was reshaped), Available() stays false and every Site-B
    // consumer degrades gracefully instead of dereferencing a stale RVA.
    //
    //   WaitForJobTask body (the bytes we anchor on, after the version-
    //   variable `mov rax,[rip+disp]` prologue):
    //     mov  r8d, ecx          44 8B C1
    //     mov  rcx, [rax + 8]    48 8B 48 08
    //     mov  rcx, [rcx+r8*8]   4A 8B 0C C1
    //     test rcx, rcx          48 85 C9
    //     je   short             74
    struct Anchors {
        bool           resolved          = false;

        std::uintptr_t moduleBase        = 0;
        std::uintptr_t moduleSize        = 0;
        const wchar_t* moduleName        = L"";

        // Absolute VAs in the running process.
        std::uintptr_t waitForJobTask    = 0;  // function entry
        std::uintptr_t singletonBSlot    = 0;  // address of the global ptr slot

        // RVAs (module-relative) for the report header.
        std::uintptr_t waitForJobTaskRVA = 0;
        std::uintptr_t singletonBSlotRVA = 0;

        // ---- Site-A worker-ack wait (SE id 34554 lock primitive) --------
        // Resolved at runtime by the version-stable body signature
        //   test rbx,rbx / je / cmp [rbx+0x6c],1 / jne / mov rcx,[rbx+0x60]
        // which is byte-identical on SE 1.5.97 and AE 1.6.1170 (only the
        // INFINITE setup differs: SE `or edx,-1`, AE `mov edx,-1`). A match
        // proves the Singleton-A layout (+0x60 handle, +0x68 work-id,
        // +0x6c pending) is the same on the runtime. Singleton-A is derived
        // from the function's `mov rbx,[rip+disp]` prologue; the lock-return
        // (the saved RIP the stack scan looks for) is the instruction after
        // the WaitForSingleObjectEx `call [rip]`.
        bool           siteAResolved      = false;
        std::uintptr_t siteALockFn        = 0;  // function entry VA
        std::uintptr_t siteALockFnEnd     = 0;  // exclusive bound for rip-in-fn
        std::uintptr_t siteALockReturn    = 0;  // insn after WFSOEx call
        std::uintptr_t singletonASlot     = 0;  // address of Singleton-A slot
        std::uintptr_t siteALockFnRVA     = 0;
        std::uintptr_t siteALockReturnRVA = 0;
        std::uintptr_t singletonASlotRVA  = 0;

        // ---- Main::Update saved return addresses ------------------------
        // For the stack-scan corroboration in the long-form audit. Resolved
        // from anchors only: the unique `call -> Site-A lock fn` pins the
        // Main::Update Site-A return and locates Main::Update; the Site-B
        // returns are the `call -> WaitForJobTask` sites within +-0x1000 of
        // it (2 on both SE and AE).
        std::uintptr_t mainUpdateRetA      = 0;
        std::uintptr_t mainUpdateRetB[4]   = {0, 0, 0, 0};
        std::size_t    mainUpdateRetBCount = 0;

        // ---- BSSpinLock spin-retry sites --------------------------------
        // The instruction after the yield `call [rip]` inside BSSpinLock's
        // spin loop. Threads parked there are spinning on a lock. SE has one
        // such site; AE has two (the canonical lock plus a sibling), so we
        // collect all and the owner search tests every one.
        bool           spinResolved     = false;
        std::uintptr_t spinRetAddr[4]    = {0, 0, 0, 0};
        std::size_t    spinRetCount      = 0;

        // ---- Render-side Site-A (SE id 34557 task-join loop) ------------
        // The render/worker thread's analog of main's Site-A. The engine's
        // parallel cloth dispatch (driven by Faster HDT-SMP) runs a worker
        // loop (SE id 34567) that, per work item, calls a per-task join
        // function (SE id 34557) which waits INFINITE on each dispatched
        // sub-task's completion before signalling the Singleton-A worker-ack
        // [+0x60]. When a sub-task's worker is stuck/gone, the render thread
        // parks forever inside id 34557 and never acks -- the render-side
        // counterpart of the main-side worker-ack deadlock (case-study 29
        // §6, first captured in freeze_2026-06-24_053440). We anchor on
        // id 34557 because its prologue
        //   push rbx; push rsi; push rdi; sub rsp,0x30; mov byte [rcx+0x71],1
        // (40 53 56 57 48 83 EC 30 C6 41 71 01) is byte-unique in .text, and
        // the captured render stack always shows a return address inside it.
        // A thread parked in a kernel wait with a saved return address in
        // [renderTaskFn, renderTaskFnEnd) is parked in the render-side
        // Site-A join. The function takes its Singleton-A in rcx (no
        // rip-relative slot), so there is nothing to derive beyond the range.
        bool           renderSiteAResolved = false;
        std::uintptr_t renderTaskFn        = 0;  // id 34557 entry
        std::uintptr_t renderTaskFnEnd     = 0;  // exclusive bound (after ret)
        std::uintptr_t renderTaskFnRVA     = 0;
    };

    // Resolve once (idempotent). Safe to call from SKSEPlugin_Load onward;
    // the game module is mapped by then. Logs the outcome.
    void Init();

    [[nodiscard]] const Anchors& Get() noexcept;

    // True iff the signature scan found WaitForJobTask AND derived a
    // non-null Singleton-B slot. Site-B consumers gate on this.
    [[nodiscard]] bool Available() noexcept;

    // True iff the Site-A lock primitive + Singleton-A were resolved.
    [[nodiscard]] bool SiteAAvailable() noexcept;

    // True iff at least one BSSpinLock spin-retry site was resolved.
    [[nodiscard]] bool SpinAvailable() noexcept;

    // True iff the render-side Site-A join function (id 34557) was resolved.
    [[nodiscard]] bool RenderSiteAAvailable() noexcept;

    // Human-readable one-liner for the Site-B anchors (report headers/logs).
    [[nodiscard]] const char* DiagnosticString();

    // Human-readable one-liner for the Site-A / spin-retry anchors.
    [[nodiscard]] const char* SiteADiagnosticString();

}
