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
    };

    // Resolve once (idempotent). Safe to call from SKSEPlugin_Load onward;
    // the game module is mapped by then. Logs the outcome.
    void Init();

    [[nodiscard]] const Anchors& Get() noexcept;

    // True iff the signature scan found WaitForJobTask AND derived a
    // non-null Singleton-B slot. Site-B consumers gate on this.
    [[nodiscard]] bool Available() noexcept;

    // Human-readable one-liner for report headers / logs.
    [[nodiscard]] const char* DiagnosticString();

}
