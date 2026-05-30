#include "PCH.h"
#include "Config.h"

#include <toml++/toml.h>

#include <ShlObj.h>

namespace FreezeLogger::Config {

    namespace {

        Root g_config{};

        std::filesystem::path PluginsDir() {
            // Game's Data/SKSE/Plugins is alongside the running EXE: <game>/Data/SKSE/Plugins.
            wchar_t modulePath[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
            std::filesystem::path exe{modulePath};
            return exe.parent_path() / "Data" / "SKSE" / "Plugins";
        }

        std::filesystem::path DocumentsSkseDir() {
            PWSTR documents = nullptr;
            if (FAILED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) {
                return {};
            }
            std::filesystem::path p{documents};
            ::CoTaskMemFree(documents);
            return p / "My Games" / "Skyrim Special Edition" / "SKSE";
        }

        template <typename T>
        T GetOr(const toml::table& tbl, std::string_view key, T fallback) {
            if (auto node = tbl.get(key)) {
                if (auto v = node->value<T>()) {
                    return *v;
                }
            }
            return fallback;
        }

        std::string GetStringOr(const toml::table& tbl, std::string_view key, std::string fallback) {
            if (auto node = tbl.get(key)) {
                if (auto v = node->value<std::string>()) {
                    return *v;
                }
            }
            return fallback;
        }

        void Parse(const toml::table& root, Root& out) {
            if (auto* w = root.get_as<toml::table>("watchdog")) {
                out.watchdog.threshold_ms        = GetOr<std::uint32_t>(*w, "threshold_ms",        out.watchdog.threshold_ms);
                out.watchdog.check_interval_ms   = GetOr<std::uint32_t>(*w, "check_interval_ms",   out.watchdog.check_interval_ms);
                out.watchdog.snapshot_cooldown_s = GetOr<std::uint32_t>(*w, "snapshot_cooldown_s", out.watchdog.snapshot_cooldown_s);
                out.watchdog.annotate_on_resolve = GetOr<bool>         (*w, "annotate_on_resolve", out.watchdog.annotate_on_resolve);
            }
            if (auto* s = root.get_as<toml::table>("snapshot")) {
                out.snapshot.include_threads      = GetOr<bool>         (*s, "include_threads",      out.snapshot.include_threads);
                out.snapshot.include_modules      = GetOr<bool>         (*s, "include_modules",      out.snapshot.include_modules);
                out.snapshot.include_papyrus      = GetOr<bool>         (*s, "include_papyrus",      out.snapshot.include_papyrus);
                out.snapshot.include_animgraph    = GetOr<bool>         (*s, "include_animgraph",    out.snapshot.include_animgraph);
                out.snapshot.include_engine       = GetOr<bool>         (*s, "include_engine",       out.snapshot.include_engine);
                out.snapshot.include_system       = GetOr<bool>         (*s, "include_system",       out.snapshot.include_system);
                out.snapshot.include_ringbuffer   = GetOr<bool>         (*s, "include_ringbuffer",   out.snapshot.include_ringbuffer);
                out.snapshot.max_threads          = GetOr<std::uint32_t>(*s, "max_threads",          out.snapshot.max_threads);
                out.snapshot.max_frames_per_stack = GetOr<std::uint32_t>(*s, "max_frames_per_stack", out.snapshot.max_frames_per_stack);
            }
            if (auto* r = root.get_as<toml::table>("ringbuffer")) {
                out.ringbuffer.papyrus_lines = GetOr<std::uint32_t>(*r, "papyrus_lines", out.ringbuffer.papyrus_lines);
                out.ringbuffer.skse_events   = GetOr<std::uint32_t>(*r, "skse_events",   out.ringbuffer.skse_events);
            }
            if (auto* m = root.get_as<toml::table>("minidump")) {
                out.minidump.enabled       = GetOr<bool>         (*m, "enabled",       out.minidump.enabled);
                out.minidump.flags         = GetStringOr         (*m, "flags",         out.minidump.flags);
                out.minidump.retain_last_n = GetOr<std::uint32_t>(*m, "retain_last_n", out.minidump.retain_last_n);
            }
            if (auto* o = root.get_as<toml::table>("output")) {
                auto dir = GetStringOr(*o, "directory", "");
                if (!dir.empty()) {
                    out.output.directory = dir;
                }
                out.output.keep_last_n_reports = GetOr<std::uint32_t>(*o, "keep_last_n_reports", out.output.keep_last_n_reports);
            }
            if (auto* s = root.get_as<toml::table>("symbols")) {
                out.symbols.use_ms_symbol_server = GetOr<bool>(*s, "use_ms_symbol_server", out.symbols.use_ms_symbol_server);
                auto cache = GetStringOr(*s, "cache_directory", "");
                if (!cache.empty()) {
                    out.symbols.cache_directory = cache;
                }
            }
            if (auto* l = root.get_as<toml::table>("logging")) {
                out.logging.level = GetStringOr(*l, "level", out.logging.level);
            }
            if (auto* t = root.get_as<toml::table>("test_mode")) {
                out.test_mode.capture_on_pause = GetOr<bool>         (*t, "capture_on_pause", out.test_mode.capture_on_pause);
                out.test_mode.hotkey_vk        = GetOr<std::uint32_t>(*t, "hotkey_vk",        out.test_mode.hotkey_vk);
            }
        }

    }

    void Load() {
        const auto path = PluginsDir() / "FreezeLogger.toml";
        if (!std::filesystem::exists(path)) {
            logs::info("FreezeLogger.toml not found at '{}', using defaults.", path.string());
            return;
        }
        try {
            const auto tbl = toml::parse_file(path.string());
            Parse(tbl, g_config);
            logs::info("Loaded config from '{}'.", path.string());
        } catch (const toml::parse_error& e) {
            logs::error("Failed to parse FreezeLogger.toml: {}", e.what());
        }
    }

    const Root& Get() noexcept { return g_config; }

    std::filesystem::path ResolvedOutputDir() {
        if (!g_config.output.directory.empty()) {
            return g_config.output.directory;
        }
        auto base = DocumentsSkseDir();
        if (base.empty()) return {};
        return base / "FreezeLogger";
    }

    std::filesystem::path ResolvedSymbolCacheDir() {
        if (!g_config.symbols.cache_directory.empty()) {
            return g_config.symbols.cache_directory;
        }
        auto out = ResolvedOutputDir();
        if (out.empty()) return {};
        return out / "symbols";
    }

}
