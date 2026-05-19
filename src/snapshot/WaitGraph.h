#pragma once
#include <ostream>

namespace FreezeLogger::Snapshot::WaitGraph {

    // Cross-cuts the per-thread snapshot: for every thread parked in
    // WaitForSingleObject*, ask "which kernel handle is it waiting on,
    // and is anyone else holding/waiting on the same one?". Builds a
    // small handle-centric table and prints it once. Read-only — no
    // wait is consumed by the probe.
    void Write(std::ostream& a_os);

}
