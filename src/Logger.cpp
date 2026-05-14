#include "PCH.h"
#include "Logger.h"

#include <spdlog/sinks/basic_file_sink.h>

namespace {

    spdlog::level::level_enum ParseLevel(std::string_view a_str) {
        if (a_str == "trace") return spdlog::level::trace;
        if (a_str == "debug") return spdlog::level::debug;
        if (a_str == "info")  return spdlog::level::info;
        if (a_str == "warn")  return spdlog::level::warn;
        if (a_str == "error") return spdlog::level::err;
        return spdlog::level::info;
    }

}

namespace FreezeLogger::Logger {

    void Init(std::string_view a_levelString) {
        const auto logsDir = SKSE::log::log_directory();
        if (!logsDir) {
            return;
        }

        const auto logPath = *logsDir / "FreezeLogger.log";

        auto sink   = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            logPath.string(), /*truncate=*/true);
        auto logger = std::make_shared<spdlog::logger>("FreezeLogger", std::move(sink));

        const auto level = ParseLevel(a_levelString);
        logger->set_level(level);
        logger->flush_on(level);

        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    }

}
