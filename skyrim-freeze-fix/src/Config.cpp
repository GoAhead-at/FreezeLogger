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

            g_settings.phase4_defer_enabled =
                tbl["phase4_defer"]["enabled"].value_or(g_settings.phase4_defer_enabled);
            g_settings.phase4_defer_diagnostic_logging =
                tbl["phase4_defer"]["diagnostic_logging"].value_or(
                    g_settings.phase4_defer_diagnostic_logging);

            g_settings.jwb_enabled =
                tbl["job_wait_breaker"]["enabled"].value_or(g_settings.jwb_enabled);
            g_settings.jwb_detect_only =
                tbl["job_wait_breaker"]["detect_only"].value_or(g_settings.jwb_detect_only);
            g_settings.jwb_dwell_threshold_ms = static_cast<std::uint32_t>(
                tbl["job_wait_breaker"]["dwell_threshold_ms"].value_or<std::int64_t>(
                    g_settings.jwb_dwell_threshold_ms));
            g_settings.jwb_poll_interval_ms = static_cast<std::uint32_t>(
                tbl["job_wait_breaker"]["poll_interval_ms"].value_or<std::int64_t>(
                    g_settings.jwb_poll_interval_ms));
            g_settings.jwb_recheck_window_ms = static_cast<std::uint32_t>(
                tbl["job_wait_breaker"]["recheck_window_ms"].value_or<std::int64_t>(
                    g_settings.jwb_recheck_window_ms));
            g_settings.jwb_diagnostic_logging =
                tbl["job_wait_breaker"]["diagnostic_logging"].value_or(
                    g_settings.jwb_diagnostic_logging);

            logs::info(
                "Config loaded from {}: enabled={}, stats_interval_s={}, "
                "phase4_defer_enabled={}, "
                "phase4_defer_diagnostic_logging={}, "
                "jwb_enabled={}, jwb_detect_only={}, "
                "jwb_dwell_threshold_ms={}, jwb_poll_interval_ms={}, "
                "jwb_recheck_window_ms={}, jwb_diagnostic_logging={}",
                path.string(),
                g_settings.enabled,
                g_settings.stats_interval_s,
                g_settings.phase4_defer_enabled,
                g_settings.phase4_defer_diagnostic_logging,
                g_settings.jwb_enabled,
                g_settings.jwb_detect_only,
                g_settings.jwb_dwell_threshold_ms,
                g_settings.jwb_poll_interval_ms,
                g_settings.jwb_recheck_window_ms,
                g_settings.jwb_diagnostic_logging);
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
