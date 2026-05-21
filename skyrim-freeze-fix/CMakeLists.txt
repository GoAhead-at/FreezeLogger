cmake_minimum_required(VERSION 3.26)

project(WorkerSpinLockFix
    VERSION       1.0.0
    DESCRIPTION   "SKSE plugin for Skyrim SE 1.5.97 that breaks an AB-BA spinlock inversion in the engine's worker dispatcher at runtime. A surgical inline hook on BSSpinLock::Acquire (id 12210) builds a per-thread wait-for graph for two specific engine BSSpinLock globals; on a confirmed cycle the offending lock's state field is force-released via InterlockedCompareExchange so the spinning thread can proceed."
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
