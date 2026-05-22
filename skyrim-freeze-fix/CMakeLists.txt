cmake_minimum_required(VERSION 3.26)

project(WorkerSpinLockFix
    VERSION       2.0.0
    DESCRIPTION   "SKSE plugin for Skyrim SE 1.5.97 that breaks an AB-BA spinlock inversion in the engine's worker dispatcher. v2.0 adds a structural fix on top of the v1.0 runtime breaker: three inline hooks on the LockA acquirer (id 19369) and the two LockB acquirers (id 40333 / id 40334) defer LockB acquires when LockA is held by the current thread, so the AB-BA cycle simply cannot form. The v1.0 runtime breaker (surgical hook on BSSpinLock::Acquire id 12210, per-thread wait-for graph, time-based confirmation flow, force-release via InterlockedCompareExchange) is retained as defence-in-depth."
    LANGUAGES     CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE  CONFIG REQUIRED)
find_package(tomlplusplus  CONFIG REQUIRED)
find_package(safetyhook    CONFIG REQUIRED)

set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/Logger.cpp
    src/Hooks.cpp
    src/Stats.cpp
    src/AcquireHook.cpp
    src/WaitGraph.cpp
    src/Breaker.cpp
    src/Phase4Defer.cpp
    src/Reaper.cpp
    src/TestMode.cpp
)

set(HEADERS
    src/PCH.h
    src/Config.h
    src/Logger.h
    src/Hooks.h
    src/Stats.h
    src/AcquireHook.h
    src/WaitGraph.h
    src/Breaker.h
    src/Phase4Defer.h
    src/Reaper.h
    src/TestMode.h
)

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
        safetyhook::safetyhook
)

target_compile_definitions(${PROJECT_NAME}
    PRIVATE
        WSLF_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        WSLF_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        WSLF_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W4
        /permissive-
        /Zc:preprocessor
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /MP
        /Zi
        /wd4324  # struct padded due to alignment specifier (intentional)
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG:FULL
        "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>"
    )
endif()
