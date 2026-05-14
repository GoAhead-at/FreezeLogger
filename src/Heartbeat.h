#pragma once

#include <atomic>
#include <cstdint>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace FreezeLogger::Heartbeat {

    inline std::atomic<std::uint64_t> g_main{0};
    inline std::atomic<std::uint64_t> g_render{0};

    // OS thread IDs of the threads that own each heartbeat. Captured once on
    // first tick (zero-write race is benign — both writers store the same
    // value because each hook only ever runs on its owning thread). The
    // snapshot's Threads section uses these to label `[main game thread]`
    // and `[render thread]` rows.
    inline std::atomic<std::uint32_t> g_mainTid{0};
    inline std::atomic<std::uint32_t> g_renderTid{0};

    inline std::uint64_t Now() noexcept {
#ifdef _WIN32
        return ::GetTickCount64();
#else
        return 0;
#endif
    }

    inline void TickMain() noexcept {
        g_main.store(Now(), std::memory_order_relaxed);
#ifdef _WIN32
        if (g_mainTid.load(std::memory_order_relaxed) == 0) {
            g_mainTid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        }
#endif
    }

    inline void TickRender() noexcept {
        g_render.store(Now(), std::memory_order_relaxed);
#ifdef _WIN32
        if (g_renderTid.load(std::memory_order_relaxed) == 0) {
            g_renderTid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        }
#endif
    }

    inline std::uint64_t Main() noexcept {
        return g_main.load(std::memory_order_relaxed);
    }

    inline std::uint64_t Render() noexcept {
        return g_render.load(std::memory_order_relaxed);
    }

    inline std::uint32_t MainTid() noexcept {
        return g_mainTid.load(std::memory_order_relaxed);
    }

    inline std::uint32_t RenderTid() noexcept {
        return g_renderTid.load(std::memory_order_relaxed);
    }

    // Test-only seam: explicit setters used by unit tests with a virtual clock.
    namespace test {
        inline void SetMain(std::uint64_t v) noexcept   { g_main.store(v, std::memory_order_relaxed); }
        inline void SetRender(std::uint64_t v) noexcept { g_render.store(v, std::memory_order_relaxed); }
        inline void Reset() noexcept {
            g_main.store(0);
            g_render.store(0);
            g_mainTid.store(0);
            g_renderTid.store(0);
        }
    }

}
