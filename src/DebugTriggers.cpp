#include "PCH.h"
#include "DebugTriggers.h"

#if FL_DEBUG_TRIGGERS_ENABLED

#include "Heartbeat.h"

namespace FreezeLogger::DebugTriggers {

    namespace {

        // Mode flags
        std::atomic<bool>           g_fakeMode{false};
        std::atomic<bool>           g_fakePaused{false};

        // Trigger queue for the OnMainTick path (used when fakeMode == false)
        std::atomic<bool>           g_pendingStall{false};
        std::atomic<std::uint32_t>  g_stallDurationS{10};

        // Threads
        std::jthread g_envWatcher;
        std::jthread g_hotkeyWatcher;
        std::jthread g_fakeHeartbeat;

        std::uint32_t ReadEnvU32(const char* a_name, std::uint32_t a_fallback) {
            char buf[32] = {};
            std::size_t len = 0;
            if (::getenv_s(&len, buf, sizeof(buf), a_name) == 0 && len > 0) {
                try {
                    return static_cast<std::uint32_t>(std::stoul(buf));
                } catch (...) {
                }
            }
            return a_fallback;
        }

        bool ReadEnvBool(const char* a_name) {
            char buf[8] = {};
            std::size_t len = 0;
            if (::getenv_s(&len, buf, sizeof(buf), a_name) == 0 && len > 0) {
                return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' ||
                       buf[0] == 't' || buf[0] == 'T';
            }
            return false;
        }

        // Induces a stall using whichever path is active.
        void InduceStall(std::uint32_t a_durationS) {
            if (g_fakeMode.load()) {
                logs::warn("DebugTriggers: pausing fake heartbeat for {}s.", a_durationS);
                g_fakePaused.store(true);
                std::this_thread::sleep_for(std::chrono::seconds(a_durationS));
                g_fakePaused.store(false);
                logs::info("DebugTriggers: fake heartbeat resumed.");
            } else {
                g_stallDurationS.store(a_durationS);
                g_pendingStall.store(true);
                logs::warn("DebugTriggers: queuing main-thread stall ({}s) for next OnMainTick.",
                           a_durationS);
            }
        }

        void FakeHeartbeatLoop(std::stop_token a_stopToken) {
            logs::info("DebugTriggers: fake heartbeat thread started (100ms tick).");
            while (!a_stopToken.stop_requested()) {
                if (!g_fakePaused.load(std::memory_order_relaxed)) {
                    Heartbeat::TickMain();
                    Heartbeat::TickRender();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            logs::info("DebugTriggers: fake heartbeat thread stopping.");
        }

        void EnvWatcher(std::stop_token a_stopToken) {
            const auto afterS    = ReadEnvU32("FL_TEST_FREEZE_AFTER_S", 0);
            const auto durationS = ReadEnvU32("FL_TEST_FREEZE_DURATION_S", 10);
            if (afterS == 0) {
                return;
            }
            logs::info("DebugTriggers: env-var freeze armed: in {}s for {}s.",
                       afterS, durationS);

            for (std::uint32_t i = 0; i < afterS; ++i) {
                if (a_stopToken.stop_requested()) return;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            InduceStall(durationS);
        }

        void HotkeyWatcher(std::stop_token a_stopToken) {
            bool wasDown = false;
            while (!a_stopToken.stop_requested()) {
                const bool isDown = (::GetAsyncKeyState(VK_PAUSE) & 0x8000) != 0;
                if (isDown && !wasDown) {
                    logs::warn("DebugTriggers: VK_PAUSE pressed.");
                    InduceStall(10);
                }
                wasDown = isDown;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

    }

    void Start() {
        g_fakeMode.store(ReadEnvBool("FL_FAKE_HEARTBEAT"));

        if (g_fakeMode.load()) {
            logs::warn("DebugTriggers: FL_FAKE_HEARTBEAT=1 — running without real hooks. "
                       "Synthetic stalls will pause a fake heartbeat thread instead of "
                       "blocking Main::Update. Use this to validate the watchdog/snapshot "
                       "pipeline before pinning hook IDs.");
            g_fakeHeartbeat = std::jthread(FakeHeartbeatLoop);
        } else {
            logs::info("DebugTriggers: enabled. Hotkey: VK_PAUSE. "
                       "Env vars: FL_TEST_FREEZE_AFTER_S, FL_TEST_FREEZE_DURATION_S.");
        }

        g_envWatcher    = std::jthread(EnvWatcher);
        g_hotkeyWatcher = std::jthread(HotkeyWatcher);
    }

    void OnMainTick() {
        if (g_fakeMode.load(std::memory_order_relaxed)) {
            return;  // fake-heartbeat path induces stalls on its own thread
        }
        if (g_pendingStall.exchange(false)) {
            const auto seconds = g_stallDurationS.load();
            logs::warn("DebugTriggers: deliberately stalling main thread for {}s.", seconds);
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
        }
    }

}

#else

namespace FreezeLogger::DebugTriggers {
    void Start()      {}
    void OnMainTick() {}
}

#endif
