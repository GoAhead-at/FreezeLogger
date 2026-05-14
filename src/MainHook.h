#pragma once

namespace FreezeLogger::MainHook {

    // Installs a trampoline over the per-frame Main::Update boundary.
    // Each invocation bumps Heartbeat::g_main, then tail-calls the original.
    void Install();

}
