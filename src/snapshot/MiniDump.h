#pragma once
#include <filesystem>
#include <ostream>

namespace FreezeLogger::Snapshot::MiniDump {

    // Writes a minidump under <a_outputDir>/minidumps/freeze_<ts>.dmp using
    // the flag set from Config::Get().minidump.flags. Logs the result to
    // a_os and to the plugin log. No-op if Config().minidump.enabled is false
    // (caller is expected to gate, but we double-check).
    void Write(std::ostream& a_os, const std::filesystem::path& a_outputDir);

}
