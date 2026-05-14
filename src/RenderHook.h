#pragma once

namespace FreezeLogger::RenderHook {

    // Installs a trampoline over BSGraphics::Renderer::Present (or the
    // per-frame end-of-render-submit boundary). Each invocation bumps
    // Heartbeat::g_render, then tail-calls the original.
    void Install();

}
