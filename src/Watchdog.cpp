#include "PCH.h"
#include "Watchdog.h"

#include "Config.h"
#include "Heartbeat.h"
#include "Reporter.h"

namespace FreezeLogger::Watchdog {

    namespace {

        std::jthread g_thread;
        std::atomic<bool> g_running{false};

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

        void Loop(std::stop_token a_stopToken) {
            const auto& cfg = Config::Get().watchdog;

            State state{};

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
    }

    void Stop() {
        if (!g_running.exchange(false)) {
            return;
        }
        if (g_thread.joinable()) {
            g_thread.request_stop();
            g_thread.join();
        }
    }

}
