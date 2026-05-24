cmake_minimum_required(VERSION 3.26)

project(WorkerSpinLockFix
    VERSION       2.0.3
    DESCRIPTION   "SKSE plugin for Skyrim SE 1.5.97 that breaks an AB-BA spinlock inversion in the engine's worker dispatcher. v2.0 adds a structural fix on top of the v1.0 runtime breaker: three inline hooks on the LockA acquirer (id 19369) and the two LockB acquirers (id 40333 / id 40334) defer LockB acquires when LockA is held by the current thread, so the AB-BA cycle simply cannot form. v2.0.1 fixes a regression in scripted-animation activators (skyshards being the most visible case): v2.0.0's wrap on id 19369 was declared void and silently dropped its bool return value, so callers consistently saw 'failed' and skipped their downstream work. v2.0.1's wrap is bool-returning and propagates the original's return via unsafe_call<bool>. As defensive scaffolding, v2.0.1 also splits the deferred operation so the kInTempChangeList bit toggle (bit 9 of [actor+0xe0]) runs synchronously in the gate (single atomic fetch_or / fetch_and) while only the LockB-protected bucket-array update stays deferred. v2.0.1 also rebases the LockB-acquirer gates from function-wraps onto two surgical Trampoline::write_call<5> patches at id 36016+0xdcb (id 40334) and id 19372+0x606 (id 40333), the only call sites in the binary that reach the LockB acquirers while LockA is held; the function entries stay pristine, so other mods that hook id 40333 / id 40334 continue to work. v2.0.3 redesigns the optional stale-owner reaper so it consumes a snapshot of the WaitGraph populated by the AcquireHook slow path instead of suspending engine threads via SuspendThread + GetThreadContext + Toolhelp32 enumeration; the runtime path no longer suspends any engine thread for any reason. The v1.0 runtime breaker (surgical hook on BSSpinLock::Acquire id 12210, per-thread wait-for graph, time-based confirmation flow, force-release via InterlockedCompareExchange) is retained as defence-in-depth."
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
