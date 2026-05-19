cmake_minimum_required(VERSION 3.26)

project(WorkerSpinLockFix
    VERSION       0.11.0
    DESCRIPTION   "Stale-owner reaper for the engine's two AB-BA-prone BSSpinLocks: when LockA/LockB stays held for >2s by a thread that no longer exists, force-release it"
    LANGUAGES     CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE  CONFIG REQUIRED)
find_package(tomlplusplus  CONFIG REQUIRED)

set(SOURCES
    src/main.cpp
    src/Config.cpp
    src/Logger.cpp
    src/Hooks.cpp
    src/Stats.cpp
)

set(HEADERS
    src/PCH.h
    src/Config.h
    src/Logger.h
    src/Hooks.h
    src/Stats.h
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
    )
    target_link_options(${PROJECT_NAME} PRIVATE
        /DEBUG:FULL
        "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF>"
    )
endif()
