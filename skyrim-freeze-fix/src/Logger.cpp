#include "PCH.h"
#include "Logger.h"

namespace WorkerSpinLockFix::Logger {

    namespace {
        constexpr std::string_view kPluginName = "WorkerSpinLockFix";
    }

    std::filesystem::path LogDirectory() {
        auto base = SKSE::log::log_directory();
        if (!base.has_value()) {
            return std::filesystem::current_path();
        }
        auto dir = *base / kPluginName;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    void Init() {
        const auto path = LogDirectory() / "WorkerSpinLockFix.log";

        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            path.string(), /*truncate*/ true);

        auto logger = std::make_shared<spdlog::logger>("global", std::move(sink));
        logger->set_level(spdlog::level::info);
        logger->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

        logs::info("WorkerSpinLockFix v{}.{}.{} logger initialized.",
            WSLF_VERSION_MAJOR, WSLF_VERSION_MINOR, WSLF_VERSION_PATCH);
        logs::info("Log file: {}", path.string());
    }

}
