#pragma once

namespace FreezeLogger::DebugTriggers {

    // Starts the debug trigger subsystem (hotkey listener + env-var one-shot,
    // optional fake-heartbeat mode). Calls into this whole namespace are
    // no-ops in release builds because every body is `#if FL_DEBUG_TRIGGERS_ENABLED`
    // gated.
    //
    // Recognized environment variables (debug builds only):
    //   FL_FAKE_HEARTBEAT=1
    //       Spawns a thread that bumps both heartbeats every 100 ms,
    //       independently of MainHook/RenderHook. Lets you test the watchdog
    //       and snapshot pipeline before the real hook IDs are pinned.
    //   FL_TEST_FREEZE_AFTER_S=N
    //       After kDataLoaded + N seconds, induce a synthetic stall.
    //   FL_TEST_FREEZE_DURATION_S=M
    //       Stall length in seconds (default 10).
    //
    // Hotkey: VK_PAUSE always triggers a 10 s stall.
    void Start();

    // Called from MainHook on every Main::Update. Performs the deliberate
    // sleep when an active trigger says so. No-op when fake-heartbeat mode
    // is active (the stall is induced on the fake-heartbeat thread instead).
    void OnMainTick();

}
