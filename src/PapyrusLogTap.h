#pragma once

namespace FreezeLogger::PapyrusLogTap {

    // Installs a BSTEventSink<BSScript::LogEvent> on the Papyrus VM via
    // its public RegisterForLogEvent API. Every log line that goes through
    // BSScript::ErrorLogger (i.e. everything that ends up in
    // Papyrus.0.log, including Debug.Trace, error frames, warnings) is
    // mirrored into the freeze-report ring buffer, channel Papyrus.
    //
    // No engine vtable patches and no hand-rolled hooks: the API is
    // documented and stable across SE/AE in CommonLibSSE-NG.
    //
    // Call once at kDataLoaded (after the VM exists). Idempotent.
    void Install();

}
