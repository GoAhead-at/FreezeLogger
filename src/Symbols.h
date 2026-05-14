#pragma once

#include <Windows.h>
#include <DbgHelp.h>

#include <mutex>
#include <string>
#include <string_view>

namespace FreezeLogger::Symbols {

    // Initializes DbgHelp once with the configured search path
    // (local cache + MS public symbol server, depending on config).
    void Init();

    // Composed search path string used by SymInitialize and emitted in the
    // report header for downstream re-symbolication. May return an empty
    // string if Init() has not run yet.
    const std::string& SearchPath();

    // Resolves a code address to "module!symbol+0xN" or "module+0xN".
    // Thread-safe; serialized internally because DbgHelp is not.
    std::string Resolve(std::uintptr_t a_address);

    // Same as Resolve(), but assumes the caller already holds Symbols::Lock.
    // Use this from inside a stack walk where the surrounding code is holding
    // the lock for the whole DbgHelp sequence; calling Resolve() in that
    // situation would deadlock against the std::mutex (resource_deadlock_would_occur).
    std::string ResolveLocked(std::uintptr_t a_address);

    // RAII guard around the DbgHelp mutex. Snapshot code that calls
    // multiple DbgHelp functions in a row should hold this for the
    // duration of the sequence.
    class Lock {
    public:
        Lock();
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

}
