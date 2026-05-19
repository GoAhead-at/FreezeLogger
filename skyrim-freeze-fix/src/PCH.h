#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <toml++/toml.h>

namespace logs = SKSE::log;
using namespace std::literals;

#ifndef WSLF_VERSION_MAJOR
#  define WSLF_VERSION_MAJOR 0
#  define WSLF_VERSION_MINOR 0
#  define WSLF_VERSION_PATCH 0
#endif
