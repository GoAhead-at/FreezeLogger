#include "PCH.h"
#include "Config.h"
#include "Logger.h"

namespace WorkerSpinLockFix::Config {

    namespace {
        Settings g_settings;
        bool     g_initialized{ false };

        std::filesystem::path PluginDirectory() {
            std::array<wchar_t, 1024> path{};
            HMODULE handle = nullptr;
            constexpr auto flags =
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
            const auto resolved = ::GetModuleHandleExW(
                flags,
                reinterpret_cast<LPCWSTR>(&PluginDirectory),
                &handle);
            if (resolved && handle) {
                const auto length = ::GetModuleFileNameW(
                    handle, path.data(), static_cast<DWORD>(path.size()));
                if (length > 0) {
                    return std::filesystem::path(
                        std::wstring_view(path.data(), length)).parent_path();
                }
            }
            return std::filesystem::path("Data/SKSE/Plugins");
        }
    }

    std::filesystem::path ConfigPath() {
        return PluginDirectory() / "WorkerSpinLockFix.toml";
    }

    void Init() {
        const auto path = ConfigPath();

        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            logs::info("Config file not found at {} - using built-in defaults.",
                path.string());
            g_initialized = true;
            return;
        }

        try {
            const auto tbl = toml::parse_file(path.string());

            g_settings.enabled =
                tbl["plugin"]["enabled"].value_or(g_settings.enabled);
            g_settings.stats_interval_s = static_cast<std::uint32_t>(
                tbl["log"]["stats_interval_s"].value_or<std::int64_t>(
                    g_settings.stats_interval_s));

            g_settings.acquire_hook_enabled =
                tbl["acquire_hook"]["enabled"].value_or(g_settings.acquire_hook_enabled);

            g_settings.break_enabled =
                tbl["breaker"]["break_enabled"].value_or(g_settings.break_enabled);
            g_settings.confirmation_window_ms = static_cast<std::uint32_t>(
                tbl["breaker"]["confirmation_window_ms"].value_or<std::int64_t>(
                    g_settings.confirmation_window_ms));
            g_settings.log_cycle_events =
                tbl["breaker"]["log_cycle_events"].value_or(g_settings.log_cycle_events);

            g_settings.phase4_defer_enabled =
                tbl["phase4_defer"]["enabled"].value_or(g_settings.phase4_defer_enabled);

            g_settings.reaper_enabled =
                tbl["reaper"]["enabled"].value_or(g_settings.reaper_enabled);
            g_settings.reaper_interval_ms = static_cast<std::uint32_t>(
                tbl["reaper"]["interval_ms"].value_or<std::int64_t>(
                    g_settings.reaper_interval_ms));

            g_settings.test_mode_enabled =
                tbl["test_mode"]["enabled"].value_or(g_settings.test_mode_enabled);

            logs::info(
                "Config loaded from {}: enabled={}, stats_interval_s={}, "
                "acquire_hook_enabled={}, break_enabled={}, "
                "confirmation_window_ms={}, log_cycle_events={}, "
                "phase4_defer_enabled={}, "
                "reaper_enabled={}, reaper_interval_ms={}, "
                "test_mode_enabled={}",
                path.string(),
                g_settings.enabled,
                g_settings.stats_interval_s,
                g_settings.acquire_hook_enabled,
                g_settings.break_enabled,
                g_settings.confirmation_window_ms,
                g_settings.log_cycle_events,
                g_settings.phase4_defer_enabled,
                g_settings.reaper_enabled,
                g_settings.reaper_interval_ms,
                g_settings.test_mode_enabled);
        }
        catch (const toml::parse_error& e) {
            logs::warn("Config parse error at {}: {} - using built-in defaults.",
                path.string(), e.description());
        }
        catch (const std::exception& e) {
            logs::warn("Config load failed: {} - using built-in defaults.",
                e.what());
        }

        g_initialized = true;
    }

    const Settings& Get() {
        return g_settings;
    }

}
