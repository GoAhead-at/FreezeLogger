#pragma once

#include <string_view>

namespace FreezeLogger::Logger {

    // a_truncate controls whether the log file is reset. The first call at
    // load time truncates for a fresh file; the post-config re-init passes
    // false so the boot lines (and any config parse errors) already written
    // are preserved while the configured level is applied.
    void Init(std::string_view a_levelString = "info", bool a_truncate = true);

}
