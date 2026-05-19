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
            g_settings.contention_warn_ms = static_cast<std::uint32_t>(
                tbl["log"]["contention_warn_ms"].value_or<std::int64_t>(
                    g_settings.contention_warn_ms));
            g_settings.stats_interval_s = static_cast<std::uint32_t>(
                tbl["log"]["stats_interval_s"].value_or<std::int64_t>(
                    g_settings.stats_interval_s));
            g_settings.trace_each_call =
                tbl["log"]["trace_each_call"].value_or(g_settings.trace_each_call);

            logs::info("Config loaded from {}: enabled={}, contention_warn_ms={}, "
                       "stats_interval_s={}, trace_each_call={}",
                path.string(),
                g_settings.enabled,
                g_settings.contention_warn_ms,
                g_settings.stats_interval_s,
                g_settings.trace_each_call);
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
