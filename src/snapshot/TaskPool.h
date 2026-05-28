#pragma once

#include <ostream>

namespace FreezeLogger::Snapshot::TaskPool {

    // Emit the task-pool snapshot section. Compares the most recent
    // healthy baseline captured by FreezeLogger::TaskPoolBaseline (from
    // the Main::Update hook, ≈1 Hz) against the frozen-time state of
    // Skyrim's task-pool holder (Singleton-B @ SkyrimSE+0x2f26a70).
    //
    // The intent — per docs/case-study/27 §0 — is to expose exactly
    // which layer of the WaitForJobTask chain was torn down between
    // the last healthy frame and the freeze instant, so the analyst
    // can name the stuck job. See docs/spec.md §6.9.5 for the section
    // layout and intent.
    void Write(std::ostream& a_os);

}
