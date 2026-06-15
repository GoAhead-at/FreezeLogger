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
    };

    // Resolve once (idempotent). Safe to call from SKSEPlugin_Load onward.
    void Init();

    [[nodiscard]] const Anchors& Get() noexcept;

    // True iff the signature scan found WaitForJobTask AND derived a
    // non-null Singleton-B slot. The JobWaitBreaker gates on this.
    [[nodiscard]] bool Available() noexcept;

    // Human-readable one-liner for logs.
    [[nodiscard]] const char* DiagnosticString();

}
