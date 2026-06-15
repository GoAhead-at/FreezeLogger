cmake_minimum_required(VERSION 3.26)

project(WorkerSpinLockFix
    VERSION       2.3.0
    DESCRIPTION   "SKSE plugin that fixes two distinct Skyrim hard-freeze classes; installs on Skyrim SE 1.5.97 and AE 1.6.x (VR refused). LAYER 1 -- Phase4Defer (the AB-BA spinlock prevention): an inline wrap on the LockA acquirer (SE id 19369 / AE id 19796) plus two surgical Trampoline::write_call<5> patches at the cycle-hub call sites (SE id 36016+0xdcb -> id 40334, SE id 19372+0x606 -> id 40333; AE id 36991+0xf6e, id 19799+0x634) defer LockB acquires while the current thread holds LockA, so the engine's AB-BA worker-dispatcher inversion structurally cannot form. All ids resolve through REL::RelocationID{se,ae} pairs and all call-site / struct offsets are runtime-selected, so the same DLL runs on both editions; VerifyCallSite() is fail-safe (a wrong offset aborts the patch cleanly rather than corrupting code). LAYER 2 (new in v2.3.0) -- JobWaitBreaker (the WaitForJobTask lost-wakeup recovery): the second, unrelated freeze class documented in case-study 27/28, where Main::Update parks in WaitForJobTask on a manual-reset job-completion event that a producer tore down WITHOUT signalling, leaving main asleep forever with no producer alive. An inline wrap on WaitForJobTask (located by a version-independent .text body signature, with the Singleton-B job-pool slot derived from its rip-relative load) marks when main is parked and on which job index; a watchdog thread detects the lost-wakeup signature (main parked past a dwell threshold on a job slot that has been torn down to null) and, in active mode, delivers the missing signal via SetEvent on the captured wait handle so main resumes -- the job is already gone, so this is equivalent to the signal that was lost. Ships detect-only by default (logs the signature, alters nothing) until the release path is field-validated. v2.3.0 removes the v1.0 runtime breaker (AcquireHook + WaitGraph + Breaker), the stale-owner reaper, and the synthetic test harness: Phase4Defer fully prevents the AB-BA cycle (validated in the field with cycles_observed=0), so the runtime breaker and its reaper backstop were redundant and have been retired to shrink the plugin's footprint to two focused, independent fixes."
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
    src/Phase4Defer.cpp
    src/SkyrimAnchors.cpp
    src/JobWaitBreaker.cpp
)

set(HEADERS
    src/PCH.h
    src/Config.h
    src/Logger.h
    src/Hooks.h
    src/Stats.h
    src/Phase4Defer.h
    src/SkyrimAnchors.h
    src/JobWaitBreaker.h
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
