find_package(Catch2 CONFIG REQUIRED)

# Tests link only the *pure* parts of the plugin (anything that doesn't
# include PCH.h / SKSE / RE).
add_executable(FreezeLoggerTests
    test_main.cpp
    test_heartbeat.cpp
    test_ringbuffer.cpp
    test_watchdog_classify.cpp
    ../src/RingBuffer.cpp
)

target_include_directories(FreezeLoggerTests PRIVATE ../src)

target_compile_features(FreezeLoggerTests PRIVATE cxx_std_23)

target_compile_definitions(FreezeLoggerTests PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(FreezeLoggerTests PRIVATE Catch2::Catch2)

if(MSVC)
    target_compile_options(FreezeLoggerTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

include(Catch)
catch_discover_tests(FreezeLoggerTests)
