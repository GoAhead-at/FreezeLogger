#pragma once

namespace WorkerSpinLockFix::Hooks {

    // Strategy F (v0.11): no hooks. We start a background watchdog
    // thread that periodically inspects the engine's two AB-BA-prone
    // BSSpinLocks and force-releases them if their current state is
    // "held by a thread that no longer exists" - the only failure
    // mode we can safely act on without coordinating with the engine.
    //
    // The function name `Install` is preserved so main.cpp does not
    // need to change. Returns true on success, false otherwise.
    bool Install();

}
