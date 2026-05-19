#pragma once

#include <cstdint>
#include <string>

namespace FreezeLogger::AddrLib {

    // Loads meh321's Address Library .bin (format v1) and exposes a fast
    // RVA -> ID lookup. The library is the canonical RVA<->ID database
    // every SKSE plugin uses; we ship a *runtime* lookup so freeze reports
    // can resolve `SkyrimSE.exe+0x5b34fe` to `id 35565 +0x50e` without a
    // separate offline post-processor pass.
    //
    // Loading is best-effort: if the .bin is missing or malformed,
    // Lookup() falls back to printing the bare RVA. This is the same
    // graceful-degradation contract the Symbols::Init path uses for PDBs.

    struct Hit {
        bool          found    = false;
        std::uint32_t id       = 0;
        std::uintptr_t base_rva = 0;   // RVA of the function entry
        std::int64_t   delta    = 0;   // address - base_rva (signed; usually >= 0)
    };

    // Initialises the lookup table. Called from SKSEPlugin_Load. Tries
    // the standard SKSE/Plugins location relative to the Skyrim executable;
    // an explicit path (TOML override) wins.
    void Init();

    bool        Available() noexcept;
    std::string DiagnosticString();   // for the report header

    // Resolve a runtime address (any module) to the nearest Address Library
    // ID. Only addresses inside SkyrimSE.exe are matched; everything else
    // returns Hit{}.
    Hit Resolve(std::uintptr_t a_runtimeAddress) noexcept;

    // Convenience formatter. Returns "" when no hit.
    std::string FormatAnnotation(std::uintptr_t a_runtimeAddress);

}
