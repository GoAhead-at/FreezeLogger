#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace FreezeLogger::Snapshot::Verdict {

    // ===== Observations =================================================
    // POD that the cheap freeze-shape probes fill in. Kept separate from
    // the classifier so the classifier can be unit-tested with synthetic
    // inputs (no game state required).
    //
    // The fields mirror the existing detection logic in MainWaitProbe but
    // are intentionally a *strict* subset: the verdict is meant to be the
    // headline, not the audit trail.
    struct Observations {
        // Wait-site detection — same algorithm MainWaitProbe uses, run on
        // main thread's saved CONTEXT and a small stack window above RSP.
        bool inSiteA = false;   // Singleton-A id 34554 lock primitive
        bool inSiteB = false;   // Skyrim WaitForJobTask @ SkyrimSE+0xc38130

        // Singleton-B chain walk: which step (if any) of the
        // *(SkyrimSE+0x2f26a70) -> [+8][0] -> *[0] -> vtable[1] chain
        // produced a null link.
        bool siteBChainWalked = false;
        bool siteBChainOk     = false;
        int  siteBNullStep    = -1;   // 0..4, or -1 if chain intact
        // Did the chain values stay the same across two reads ~50 ms
        // apart? "false" => writer is still actively mutating, "true" =>
        // steady-state deadlock. Only meaningful when siteBChainWalked.
        bool siteBChainStable = false;

        // HDT-SMP fingerprint — observations gathered against any
        // loaded module whose base name is `hdtsmp64.dll` (case-insensitive).
        bool           hdtsmpLoaded         = false;
        std::string    hdtsmpVersion;            // FileVersion of the DLL,
                                                 // empty if not readable
        bool           hdtsmpOnMainStack    = false;
        std::uintptr_t hdtsmpFrameRva       = 0; // RVA inside hdtsmp64.dll
                                                 // for the topmost frame
                                                 // we found
        int            hdtsmpWorkerPoolIdle = 0; // number of threads parked
                                                 // with their top frame
                                                 // inside hdtsmp64.dll
        std::uintptr_t hdtsmpWorkerWaitRva  = 0; // common RVA those workers
                                                 // sit at (0 if mixed)

        // BSSpinLock AB-BA cycle — any thread spinning at the spin-retry
        // RVA AND its observed lock-owner field equals main's TID.
        bool spinlockSpinnerSeen  = false;       // a spinner exists at all
        bool spinlockOwnedByMain  = false;       // owner == main TID
    };

    // ===== Classified verdict ==========================================
    //
    // The enum identifiers are stable across releases for source / log-grep
    // compatibility; the human-readable labels rendered into reports are
    // reclassified in v0.2.1 after the Faster HDT-SMP-UP maintainer
    // identified Site-B as Skyrim's `WaitForJobTask` ("waiting on jobs
    // before rendering"), not a Papyrus event-source wait. The historical
    // identifier `HdtsmpSiteB` is kept because the *fingerprint* still
    // matters — a Site-B wait with an `hdtsmp64.dll` frame on main's
    // stack is the common shape — but the label now says the FSMP frame
    // is the upstream `Main::Update` hook, not the cause of the wait.
    enum class Class {
        SpinlockAbBa,          // BSSpinLock AB-BA (WorkerSpinLockFix domain)
        HdtsmpSiteB,           // Skyrim WaitForJobTask hang, FSMP on main stack
        SiteBNoHdtsmp,         // Skyrim WaitForJobTask hang, no FSMP on stack
        SiteAWorkerAck,        // Site-A worker-ack wait, no spinlock cycle
        Unrecognised,
    };

    struct Classified {
        Class       klass;
        std::string label;        // human-readable verdict line
        std::string confidence;   // "high" / "medium" / "low"
        std::string triageDoc;    // relative path under docs/
    };

    // Pure function (no I/O, no game state). Unit-testable.
    Classified Classify(const Observations& a_obs);

    // Watchdog-thread entry. Runs Observe → Classify → format and writes
    // a 10–15 line block. Safe to call from the watchdog snapshot path;
    // every probe is SEH-guarded and bounded.
    void Write(std::ostream& a_os);

    // Test seam: lets unit tests bypass Observe and feed a synthetic
    // Observations through the formatter so the renderer is exercised
    // end-to-end without a running process.
    void WriteFromObservations(std::ostream& a_os, const Observations& a_obs);

}
