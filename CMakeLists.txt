cmake_minimum_required(VERSION 3.26)

project(FreezeLogger
    VERSION       0.3.0
    DESCRIPTION   "SKSE plugin that detects and diagnoses Skyrim hard freezes"
    HOMEPAGE_URL  ""
    LANGUAGES     CXX
)

option(FL_DEBUG_TRIGGERS "Enable synthetic-stall hotkey + env-var (debug only)" OFF)
option(FL_BUILD_TESTS    "Build Catch2 unit tests"                              ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE  CONFIG REQUIRED)
find_package(tomlplusplus  CONFIG REQUIRED)

set(SOURCES
    src/main.cpp
    src/AddrLib.cpp
    src/Config.cpp
    src/Logger.cpp
    src/MainHook.cpp
    src/RenderHook.cpp
    src/Watchdog.cpp
    src/Symbols.cpp
    src/Reporter.cpp
    src/RingBuffer.cpp
    src/PapyrusLogTap.cpp
    src/SkseMessageTap.cpp
    src/DebugTriggers.cpp
    src/TaskPoolBaseline.cpp
    src/snapshot/Threads.cpp
    src/snapshot/Modules.cpp
    src/snapshot/Papyrus.cpp
    src/snapshot/AnimGraph.cpp
    src/snapshot/Engine.cpp
    src/snapshot/MainWaitProbe.cpp
    src/snapshot/System.cpp
    src/snapshot/MiniDump.cpp
    src/snapshot/TaskPool.cpp
    src/snapshot/Verdict.cpp
    src/snapshot/WaitGraph.cpp
)

set(HEADERS
    src/PCH.h
    src/AddrLib.h
    src/Config.h
    src/Heartbeat.h
    src/Logger.h
    src/MainHook.h
    src/RenderHook.h
    src/Watchdog.h
    src/Symbols.h
    src/Reporter.h
    src/RingBuffer.h
    src/PapyrusLogTap.h
    src/SkseMessageTap.h
    src/DebugTriggers.h
    src/TaskPoolBaseline.h
    src/snapshot/Threads.h
    src/snapshot/Modules.h
    src/snapshot/Papyrus.h
    src/snapshot/AnimGraph.h
    src/snapshot/Engine.h
    src/snapshot/MainWaitProbe.h
    src/snapshot/System.h
    src/snapshot/MiniDump.h
    src/snapshot/TaskPool.h
    src/snapshot/Verdict.h
    src/snapshot/WaitGraph.h
)

# add_commonlibsse_plugin is provided by the commonlibsse-ng vcpkg port.
# It builds a SHARED library, links CommonLibSSE, and emits the SKSE plugin
# version block in the right shape.
add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES ${SOURCES} ${HEADERS}
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tomlplusplus::tomlplusplus
        DbgHelp
        Psapi
        Version
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        FL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        FL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        FL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(FL_DEBUG_TRIGGERS)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FL_DEBUG_TRIGGERS_ENABLED=1)
    message(STATUS "FreezeLogger: synthetic-stall debug triggers ENABLED")
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE FL_DEBUG_TRIGGERS_ENABLED=0)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /MP
        /Zi    # Always emit a PDB. The plugin resolves its own symbols
               # against this PDB at freeze time, so we want one even in
               # Release builds.
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG:FULL
        "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>"
    )
endif()

if(FL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
