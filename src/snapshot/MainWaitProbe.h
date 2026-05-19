#pragma once
#include <ostream>

namespace FreezeLogger::Snapshot::MainWaitProbe {

    // Reads and prints the state of the Skyrim singleton that gates the
    // event-wait inside RE::Main::Update (id 35565). Discovered via
    // post-mortem analysis of freeze_2026-05-17_023447: the main thread
    // was parked at SkyrimSE+0x5765d0 (id 34554) inside an unconditional
    // WaitForSingleObjectEx(handle, INFINITE, FALSE) on a handle stored
    // inside this singleton.
    //
    // Layout deduced from disassembly of the unpacked SE 1.5.97 binary:
    //   *(uintptr_t*)(SkyrimSE.exe + 0x2f266c8) -> Singleton ptr
    //   Singleton + 0x60  : HANDLE  event handle
    //   Singleton + 0x68  : uint32  reserved (cleared before wait)
    //   Singleton + 0x6c  : uint32  pending  (1 == wait scheduled,
    //                                          cleared after wait)
    //
    // Probe is read-only and uses NtQueryEvent to inspect the event
    // state without consuming a signal. Safe to call from the watchdog
    // capture path.
    void Write(std::ostream& a_os);

}
