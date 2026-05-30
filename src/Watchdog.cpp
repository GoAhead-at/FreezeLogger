#include "PCH.h"
#include "Watchdog.h"

#include "Config.h"
#include "Heartbeat.h"
#include "Reporter.h"

namespace FreezeLogger::Watchdog {

    namespace {

        std::jthread g_thread;
        std::atomic<bool> g_running{false};

        // Optional test_mode hotkey thread (always compiled; armed only when
        // [test_mode] capture_on_pause = true). Distinct from the debug-only
        // DebugTriggers hotkey, which is #if-compiled out of release builds
        // and induces a real stall. This one writes a report on demand
        // without stalling the game.
        std::jthread g_hotkeyThread;

        void HotkeyLoop(std::stop_token a_stopToken) {
            const auto vk = Config::Get().test_mode.hotkey_vk;
            logs::warn(
                "Watchdog: test_mode capture-on-hotkey ARMED (VK 0x{:x}). "
                "Pressing it writes an on-demand report; the game is NOT "
                "stalled. Disable [test_mode] capture_on_pause for normal play.",
                vk);

            bool wasDown = false;
            while (!a_stopToken.stop_requested()) {
                const bool isDown =
                    (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
                if (isDown && !wasDown) {
                    logs::warn("Watchdog: test_mode hotkey pressed; capturing "
                               "manual report.");
                    Reporter::CaptureManual();
                }
                wasDown = isDown;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // True iff the foreground window belongs to *our* (i.e. Skyrim's)
        // process. When false, Skyrim's WinMain runs an idle-sleep path that
        // deliberately stops ticking Main::Update; treating that as a freeze
        // is a false positive (alt-tab, minimised, focus-stolen).
        bool ForegroundIsOurs() noexcept {
            HWND fg = ::GetForegroundWindow();
            if (!fg) return false;
            DWORD pid = 0;
            ::GetWindowThreadProcessId(fg, &pid);
            return pid == ::GetCurrentProcessId();
        }

        // Per-loop early-warning state. Logs ONCE when a stall crosses 50 %
        // of the threshold and ONCE more after it resolves. Independent of
        // the trip/snapshot machinery — purpose is to leave a paper trail
        // in FreezeLogger.log even when the user kills the process before
        // the full threshold elapses.
        struct EarlyWarn {
            bool          armed       = false;  // we've logged the "stale" line
            std::uint64_t logged_at   = 0;
            std::uint64_t logged_age  = 0;
        };

        void MaybeLogEarlyWarning(EarlyWarn&    a_ew,
                                  std::uint64_t a_now,
                                  std::uint64_t a_mainAge,
                                  std::uint64_t a_renderAge,
                                  std::uint32_t a_thresholdMs,
                                  bool          a_fgOurs)
        {
            const auto half = a_thresholdMs / 2;
            const auto worst = (a_mainAge > a_renderAge) ? a_mainAge : a_renderAge;
            const bool stale = a_fgOurs && worst > half;

            if (stale && !a_ew.armed) {
                a_ew.armed      = true;
                a_ew.logged_at  = a_now;
                a_ew.logged_age = worst;
                logs::info(
                    "Watchdog: heartbeat stale (mainAge={}ms, renderAge={}ms, "
                    "threshold={}ms). Still under threshold; will trip if it "
                    "persists. (Logged early so a force-kill before threshold "
                    "still leaves a record.)",
                    a_mainAge, a_renderAge, a_thresholdMs);
            } else if (!stale && a_ew.armed) {
                logs::info(
                    "Watchdog: heartbeat recovered without tripping (peak age "
                    "~{}ms before recovery).",
                    a_ew.logged_age);
                a_ew = {};
            }
        }

        void Loop(std::stop_token a_stopToken) {
            const auto& cfg = Config::Get().watchdog;

            State     state{};
            EarlyWarn earlyWarn{};

            logs::info(
                "Watchdog started (threshold={}ms, check={}ms, cooldown={}s).",
                cfg.threshold_ms, cfg.check_interval_ms, cfg.snapshot_cooldown_s);

            while (!a_stopToken.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cfg.check_interval_ms));

                const auto now    = Heartbeat::Now();
                const auto main   = Heartbeat::Main();
                const auto render = Heartbeat::Render();
                const auto fgOurs = ForegroundIsOurs();

                const auto step = Step(
                    state, now, main, render,
                    cfg.threshold_ms, cfg.snapshot_cooldown_s,
                    /*a_canTrip=*/fgOurs);

                MaybeLogEarlyWarning(earlyWarn, now,
                                     step.main_age_ms, step.render_age_ms,
                                     cfg.threshold_ms, fgOurs);

                switch (step.action) {
                case Action::EmitSnapshot:
                    logs::warn(
                        "Freeze detected: stalled={}, mainAge={}ms, renderAge={}ms",
                        ToString(step.stalled), step.main_age_ms, step.render_age_ms);
                    Reporter::CaptureAndWrite(step.stalled,
                                              step.main_age_ms,
                                              step.render_age_ms);
                    break;

                case Action::SuppressedStall:
                    logs::debug(
                        "Watchdog: stall observed (stalled={}, mainAge={}ms, renderAge={}ms) "
                        "but Skyrim window is not in foreground; suppressing snapshot "
                        "(treating as alt-tab / minimised idle).",
                        ToString(step.stalled), step.main_age_ms, step.render_age_ms);
                    break;

                case Action::AnnotateResolve:
                    logs::info("Freeze resolved after {} ms.", step.resolved_after_ms);
                    if (cfg.annotate_on_resolve) {
                        Reporter::AnnotateLatestResolved(step.resolved_after_ms);
                    }
                    break;

                case Action::None:
                    break;
                }
            }

            logs::info("Watchdog stopping.");
        }

    }

    void Start() {
        if (g_running.exchange(true)) {
            return;
        }
        g_thread = std::jthread(Loop);

        if (Config::Get().test_mode.capture_on_pause) {
            g_hotkeyThread = std::jthread(HotkeyLoop);
        }
    }

    void Stop() {
        if (!g_running.exchange(false)) {
            return;
        }
        if (g_hotkeyThread.joinable()) {
            g_hotkeyThread.request_stop();
            g_hotkeyThread.join();
        }
        if (g_thread.joinable()) {
            g_thread.request_stop();
            g_thread.join();
        }
    }

}
