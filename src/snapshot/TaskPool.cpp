#include "PCH.h"
#include "snapshot/TaskPool.h"

#include "Config.h"
#include "Heartbeat.h"
#include "SkyrimAnchors.h"
#include "Symbols.h"
#include "TaskPoolBaseline.h"

#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace FreezeLogger::Snapshot::TaskPool {

    namespace {

        // ----- SEH-bounded primitives ------------------------------------
        // Local copies — kept here so Snapshot::TaskPool stays a leaf
        // translation unit that can fault independently of any other
        // probe.
        bool TryReadQword(std::uintptr_t a_addr, std::uintptr_t& a_out) noexcept {
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        void ReadWindow(
            std::uintptr_t  a_addr,
            std::uintptr_t* a_out,
            int             a_count) noexcept
        {
            for (int i = 0; i < a_count; ++i) {
                std::uintptr_t v = 0;
                if (TryReadQword(a_addr + i * sizeof(std::uintptr_t), v)) {
                    a_out[i] = v;
                } else {
                    a_out[i] = 0;
                }
            }
        }

        // Frozen-time capture of the same shape as the baseline. SEH-safe;
        // no C++ destructors used inside.
        bool BuildFrozenSample(
            std::uintptr_t            a_slotVA,
            TaskPoolBaseline::Sample& a_out) noexcept
        {
            a_out = TaskPoolBaseline::Sample{};
            a_out.captureTickMs = ::GetTickCount64();

            std::uintptr_t singleton = 0;
            if (!TryReadQword(a_slotVA, singleton)) {
                return false;
            }
            a_out.singletonPtr = singleton;
            if (singleton != 0) {
                ReadWindow(singleton, a_out.singletonWindow,
                           TaskPoolBaseline::Sample::kSingletonWindow);
            }

            std::uintptr_t subArray = 0;
            if (singleton != 0 &&
                TryReadQword(singleton + 8, subArray))
            {
                a_out.subArrayPtr = subArray;
            }
            if (subArray != 0) {
                ReadWindow(subArray, a_out.subArrayWindow,
                           TaskPoolBaseline::Sample::kSubArrayWidth);
            }

            int populated = 0;
            if (subArray != 0) {
                for (int i = 0;
                     i < TaskPoolBaseline::Sample::kMaxEntries;
                     ++i)
                {
                    std::uintptr_t entry = 0;
                    if (!TryReadQword(
                            subArray + i * sizeof(std::uintptr_t), entry) ||
                        entry == 0)
                    {
                        continue;
                    }
                    auto& e = a_out.entries[populated];
                    e.entryPtr = entry;
                    ReadWindow(entry, e.entryWindow,
                               TaskPoolBaseline::Sample::kEntryWidth);

                    std::uintptr_t handleTable = 0;
                    if (TryReadQword(entry, handleTable) &&
                        handleTable != 0)
                    {
                        e.handleTablePtr = handleTable;
                        ReadWindow(handleTable, e.handleTableWindow,
                                   TaskPoolBaseline::Sample::kHandleTableWidth);
                    }
                    ++populated;
                }
            }
            a_out.populatedEntries = populated;
            return true;
        }

        // ----- ASCII annotation -----------------------------------------
        // Same heuristic as MainWaitProbe::DecodeQwordAsAscii — kept
        // local so we don't introduce a cross-TU dependency for a tiny
        // helper.
        std::string DecodeQwordAsAscii(std::uintptr_t a_qword) noexcept {
            if (a_qword == 0) return {};
            const auto firstByte = static_cast<unsigned char>(a_qword & 0xff);
            const bool firstLooksLikeText =
                (firstByte >= 'A' && firstByte <= 'Z') ||
                (firstByte >= 'a' && firstByte <= 'z') ||
                 firstByte == '_';
            if (!firstLooksLikeText) return {};

            std::string out;
            out.reserve(8);
            for (int i = 0; i < 8; ++i) {
                const auto byte = static_cast<unsigned char>(
                    (a_qword >> (i * 8)) & 0xff);
                if (byte == 0) break;
                if (byte < 0x20 || byte > 0x7e) return {};
                out.push_back(static_cast<char>(byte));
            }
            return out;
        }

        // ----- Renderers --------------------------------------------------
        // Side-by-side qword renderer. When the values differ, append a
        // DIFF marker on the frozen-side line. When ASCII-decodable,
        // append the decoded string (with the v0.2.1 caveat that the
        // bytes may be coincidental ASCII inside a job-id / hash).
        void WriteDiffLine(
            std::ostream&  a_os,
            std::size_t    a_offset,
            std::uintptr_t a_baseline,
            std::uintptr_t a_frozen,
            bool           a_haveBaseline)
        {
            const auto ascii = DecodeQwordAsAscii(a_baseline ? a_baseline
                                                             : a_frozen);
            const bool sameVal = a_haveBaseline && a_baseline == a_frozen;
            const char* sameStr = " (same)";
            const char* diffStr = "   <-- DIFF";

            if (a_haveBaseline) {
                a_os << std::format(
                    "    +0x{:03x}  baseline 0x{:016x}\n",
                    a_offset, a_baseline);
                a_os << std::format(
                    "            frozen   0x{:016x}{}",
                    a_frozen, sameVal ? sameStr : diffStr);
            } else {
                a_os << std::format(
                    "    +0x{:03x}  frozen   0x{:016x}",
                    a_offset, a_frozen);
            }
            if (!ascii.empty()) {
                a_os << std::format("  \"{}\"", ascii);
            }
            a_os << "\n";
        }

        void WriteWindowDiff(
            std::ostream&         a_os,
            const std::uintptr_t* a_baseline,
            const std::uintptr_t* a_frozen,
            int                   a_count,
            bool                  a_haveBaseline)
        {
            for (int i = 0; i < a_count; ++i) {
                const std::uintptr_t b = a_haveBaseline ? a_baseline[i] : 0;
                const std::uintptr_t f = a_frozen[i];
                WriteDiffLine(a_os,
                              static_cast<std::size_t>(i) * sizeof(std::uintptr_t),
                              b, f, a_haveBaseline);
            }
        }

        const TaskPoolBaseline::Sample::Entry* FindBaselineEntryByPtr(
            const TaskPoolBaseline::Sample& a_baseline,
            std::uintptr_t                  a_ptr)
        {
            if (a_ptr == 0) return nullptr;
            for (int i = 0; i < a_baseline.populatedEntries; ++i) {
                if (a_baseline.entries[i].entryPtr == a_ptr) {
                    return &a_baseline.entries[i];
                }
            }
            return nullptr;
        }

        // ================================================================
        // Stuck-job attribution
        // ----------------------------------------------------------------
        // When main is parked in WaitForJobTask on HANDLE H, that handle
        // lives in one task-pool entry's handle_table. The producer that
        // should SetEvent(H) is some worker thread currently processing a
        // job from that queue. We can't read it from main, but a worker
        // that owns the job almost always still holds a pointer to the
        // entry / dispatch struct / handle table (or the bare handle value
        // H itself) in a register or a saved stack slot. So: build a target
        // set from the frozen pool capture + main's wait handle, then scan
        // every other thread's registers and stack window for a hit and
        // dump that thread's full stack. This automates the manual
        // "cross-reference the Threads section" hint.
        // ================================================================

        constexpr std::size_t kAttribStackWindow = 0x800;   // 256 qwords
        constexpr int         kMaxTargets        = 160;

        // Heap-pointer heuristic: user-mode committed address, 8-aligned.
        bool LooksLikeHeapPtr(std::uintptr_t v) noexcept {
            return v >= 0x10000 &&
                   v < 0x00007FFFFFFFFFFFull &&
                   (v & 7) == 0;
        }

        // SEH-bounded: read main's RBX (the wait handle KERNELBASE leaves
        // in the caller's RBX) from a captured CONTEXT-free path. We pass
        // the already-read value in; this stays a pure helper.

        // SEH-bounded stack scan. Returns the index into a_targets of the
        // first matching qword in [rsp, rsp+window), or -1. Reports the
        // matched value + byte offset for the report line.
        int ScanStackForTargets(std::uintptr_t        a_rsp,
                                 const std::uintptr_t* a_targets,
                                 int                   a_count,
                                 std::size_t           a_windowBytes,
                                 std::uintptr_t&       a_outVal,
                                 std::size_t&          a_outOff) noexcept
        {
            __try {
                for (std::size_t off = 0; off < a_windowBytes;
                     off += sizeof(std::uintptr_t)) {
                    const auto v = *reinterpret_cast<volatile std::uintptr_t*>(
                        a_rsp + off);
                    for (int t = 0; t < a_count; ++t) {
                        if (v == a_targets[t]) {
                            a_outVal = v;
                            a_outOff = off;
                            return t;
                        }
                    }
                }
                return -1;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return -1;
            }
        }

        // RAII suspend/resume for one thread handle.
        struct SuspendGuard {
            HANDLE h;
            bool   ok_;
            explicit SuspendGuard(HANDLE a_h) noexcept : h(a_h) {
                ok_ = h && (::SuspendThread(h) != static_cast<DWORD>(-1));
            }
            ~SuspendGuard() { if (ok_) ::ResumeThread(h); }
            SuspendGuard(const SuspendGuard&)            = delete;
            SuspendGuard& operator=(const SuspendGuard&) = delete;
            bool ok() const noexcept { return ok_; }
        };

        // RAII close for a thread handle. Declared BEFORE any SuspendGuard so
        // that, on scope exit, the suspend guard resumes the thread first and
        // the handle is closed second.
        struct HandleGuard {
            HANDLE h;
            explicit HandleGuard(HANDLE a_h) noexcept : h(a_h) {}
            ~HandleGuard() { if (h) ::CloseHandle(h); }
            HandleGuard(const HandleGuard&)            = delete;
            HandleGuard& operator=(const HandleGuard&) = delete;
        };

        std::vector<DWORD> EnumerateThreads(DWORD a_pid) noexcept {
            std::vector<DWORD> tids;
            HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snap == INVALID_HANDLE_VALUE) return tids;
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (::Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == a_pid) {
                        tids.push_back(te.th32ThreadID);
                    }
                    te.dwSize = sizeof(te);
                } while (::Thread32Next(snap, &te));
            }
            ::CloseHandle(snap);
            return tids;
        }

        // Read main's wait handle (RBX). Suspends main briefly.
        std::uintptr_t ReadMainWaitHandle(DWORD a_mainTid) noexcept {
            if (a_mainTid == 0) return 0;
            HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_mainTid);
            if (!h) return 0;
            std::uintptr_t handle = 0;
            {
                SuspendGuard sg{h};
                if (sg.ok()) {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_INTEGER;
                    if (::GetThreadContext(h, &ctx)) {
                        handle = static_cast<std::uintptr_t>(ctx.Rbx);
                    }
                }
            }
            ::CloseHandle(h);
            return handle;
        }

        // Full symbolicated stack walk for one (already-suspended) thread.
        void DumpThreadStack(std::ostream& a_os, HANDLE a_thread) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!::GetThreadContext(a_thread, &ctx)) {
                a_os << "        <GetThreadContext failed>\n";
                return;
            }

            STACKFRAME64 frame{};
            frame.AddrPC.Mode    = AddrModeFlat;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Mode = AddrModeFlat;
            frame.AddrPC.Offset    = ctx.Rip;
            frame.AddrFrame.Offset = ctx.Rbp;
            frame.AddrStack.Offset = ctx.Rsp;

            const int maxFrames = static_cast<int>(
                std::max<std::uint32_t>(
                    1u, Config::Get().snapshot.max_frames_per_stack));
            int n = 0;

            Symbols::Lock symLock;
            while (n < maxFrames &&
                   ::StackWalk64(IMAGE_FILE_MACHINE_AMD64,
                                 ::GetCurrentProcess(), a_thread, &frame, &ctx,
                                 nullptr, ::SymFunctionTableAccess64,
                                 ::SymGetModuleBase64, nullptr)) {
                if (frame.AddrPC.Offset == 0) break;
                const auto pc = static_cast<std::uintptr_t>(frame.AddrPC.Offset);
                a_os << std::format("        #{:02} 0x{:016x}  {}\n",
                                    n, pc, Symbols::ResolveLocked(pc));
                ++n;
            }
            if (n == 0) {
                a_os << "        <stack walk produced no frames>\n";
            }
        }

        // The 16 integer registers, in the order we fill them.
        const char* const kRegNames[16] = {
            "RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
            "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15",
        };

        // Probe one thread: open + suspend + scan registers/stack for any
        // target. On a hit, emit a candidate block with the full stack.
        // Returns true iff this thread matched. All resource lifetimes are
        // RAII so there is exactly one CloseHandle and one ResumeThread per
        // thread regardless of which path we take.
        bool ProbeThread(std::ostream&               a_os,
                         DWORD                       a_tid,
                         const std::vector<std::uintptr_t>& a_tvals,
                         const std::vector<std::string>&    a_tlabels)
        {
            HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!h) return false;

            HandleGuard hg{h};            // closes last
            SuspendGuard sg{h};           // resumes first (declared after hg)
            if (!sg.ok()) return false;

            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            if (!::GetThreadContext(h, &ctx)) return false;

            const std::uintptr_t regs[16] = {
                ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx,
                ctx.Rsi, ctx.Rdi, ctx.Rbp, ctx.Rsp,
                ctx.R8,  ctx.R9,  ctx.R10, ctx.R11,
                ctx.R12, ctx.R13, ctx.R14, ctx.R15,
            };

            const int nTargets = static_cast<int>(a_tvals.size());

            int regHitReg    = -1;
            int regHitTarget = -1;
            for (int r = 0; r < 16 && regHitTarget < 0; ++r) {
                for (int t = 0; t < nTargets; ++t) {
                    if (regs[r] == a_tvals[t]) {
                        regHitReg    = r;
                        regHitTarget = t;
                        break;
                    }
                }
            }

            std::uintptr_t stackVal = 0;
            std::size_t    stackOff = 0;
            const int stackHitTarget = ScanStackForTargets(
                static_cast<std::uintptr_t>(ctx.Rsp),
                a_tvals.data(), nTargets,
                kAttribStackWindow, stackVal, stackOff);

            if (regHitTarget < 0 && stackHitTarget < 0) {
                return false;
            }

            a_os << std::format("\n    >>> Candidate producer: TID {}\n", a_tid);
            if (regHitTarget >= 0) {
                a_os << std::format(
                    "        match: {} == {} (0x{:016x})\n",
                    kRegNames[regHitReg], a_tlabels[regHitTarget],
                    a_tvals[regHitTarget]);
            }
            if (stackHitTarget >= 0) {
                a_os << std::format(
                    "        match: stack[rsp+0x{:x}] == {} (0x{:016x})\n",
                    stackOff, a_tlabels[stackHitTarget], stackVal);
            }
            a_os << "        stack:\n";
            DumpThreadStack(a_os, h);
            return true;
        }

        void WriteJobAttribution(std::ostream&                   a_os,
                                 const TaskPoolBaseline::Sample& a_frozen)
        {
            a_os << "\n";
            a_os << "  ===== Stuck-job attribution (automated producer search) =====\n";

            const auto mainTid = static_cast<DWORD>(Heartbeat::MainTid());
            const auto selfTid = ::GetCurrentThreadId();
            const auto pid     = ::GetCurrentProcessId();

            const auto mainHandle = ReadMainWaitHandle(mainTid);
            if (mainHandle == 0) {
                a_os << "    <could not read main's wait handle (RBX); "
                        "attribution skipped>\n";
                return;
            }
            a_os << std::format(
                "    Main (TID {}) wait handle: 0x{:x}\n", mainTid, mainHandle);

            // Locate the entry whose handle_table holds main's handle.
            int primaryEntry = -1;
            for (int i = 0; i < a_frozen.populatedEntries && primaryEntry < 0; ++i) {
                for (int j = 0;
                     j < TaskPoolBaseline::Sample::kHandleTableWidth; ++j) {
                    if (a_frozen.entries[i].handleTableWindow[j] == mainHandle) {
                        primaryEntry = i;
                        break;
                    }
                }
            }
            if (primaryEntry >= 0) {
                a_os << std::format(
                    "    Owning queue: entry[{}] @ 0x{:016x} "
                    "(handle_table @ 0x{:016x})\n",
                    primaryEntry, a_frozen.entries[primaryEntry].entryPtr,
                    a_frozen.entries[primaryEntry].handleTablePtr);
            } else {
                a_os << "    Owning queue: main's handle is NOT in any captured "
                        "entry handle_table\n"
                        "      (it may live past the 8-entry cap, or the table "
                        "moved). Searching all pool pointers anyway.\n";
            }

            // Build the target set (parallel value array for the SEH scan,
            // labels for the report). Pool object pointers are strong, near-
            // zero-false-positive signals; the bare handle value is a weaker
            // but still useful signal.
            std::vector<std::uintptr_t> tvals;
            std::vector<std::string>    tlabels;
            auto addTarget = [&](std::uintptr_t v, std::string label) {
                if (v == 0) return;
                if (static_cast<int>(tvals.size()) >= kMaxTargets) return;
                for (auto existing : tvals) {
                    if (existing == v) return;   // dedupe
                }
                tvals.push_back(v);
                tlabels.push_back(std::move(label));
            };

            addTarget(a_frozen.singletonPtr, "Singleton-B instance");
            addTarget(a_frozen.subArrayPtr,  "sub_array");
            for (int i = 0; i < a_frozen.populatedEntries; ++i) {
                const auto& e = a_frozen.entries[i];
                const bool prim = (i == primaryEntry);
                const char* tag = prim ? "PRIMARY " : "";
                addTarget(e.entryPtr,
                          std::format("{}entry[{}]", tag, i));
                addTarget(e.handleTablePtr,
                          std::format("{}entry[{}].handle_table", tag, i));
                for (int k = 0; k < TaskPoolBaseline::Sample::kEntryWidth; ++k) {
                    const auto v = e.entryWindow[k];
                    if (LooksLikeHeapPtr(v)) {
                        addTarget(v, std::format("{}entry[{}].field+0x{:x}",
                                                 tag, i, k * 8));
                    }
                }
            }
            // The bare handle value (a producer about to signal it holds it).
            addTarget(mainHandle, "main wait handle value");

            a_os << std::format(
                "    Scanning every thread for any of {} pool target(s) "
                "(registers + 0x{:x} bytes of stack)...\n",
                tvals.size(), kAttribStackWindow);

            auto tids = EnumerateThreads(pid);
            int  matches = 0;
            int  scanned = 0;

            for (const auto tid : tids) {
                if (tid == selfTid || tid == mainTid) continue;
                ++scanned;
                try {
                    if (ProbeThread(a_os, tid, tvals, tlabels)) {
                        ++matches;
                    }
                } catch (...) {
                    a_os << "        <attribution walk aborted for TID "
                         << tid << ">\n";
                }
            }

            a_os << "\n";
            if (matches == 0) {
                a_os << std::format(
                    "    No candidate producer found among {} scanned thread(s).\n",
                    scanned);
                a_os << "    The responsible worker holds no pool pointer in its\n";
                a_os << "    registers or top of stack — it has likely\n";
                a_os << "    already returned from the job (orphaned signal), or is\n";
                a_os << "    blocked deeper than the scan window (e.g. inside a\n";
                a_os << "    kernel IO wait with the pool pointer spilled below the\n";
                a_os << "    window). Cross-reference the Threads section for any\n";
                a_os << "    worker parked in NtCreateFile/NtReadFile or a 3rd-party\n";
                a_os << "    module.\n";
            } else {
                a_os << std::format(
                    "    {} candidate producer thread(s) reference the stuck\n"
                    "    queue. The one whose stack sits in an engine job-exec\n"
                    "    frame (BSTaskPool / job worker) above a blocking call is\n"
                    "    the most likely culprit; a PRIMARY-entry match is the\n"
                    "    strongest signal.\n",
                    matches);
            }
        }

    }   // anonymous namespace

    void Write(std::ostream& a_os) {
        if (!SkyrimAnchors::Available()) {
            a_os << "<Singleton-B anchor unavailable on this runtime; "
                    "task-pool snapshot skipped>\n";
            a_os << "  reason: " << SkyrimAnchors::DiagnosticString() << "\n";
            return;
        }
        const auto& anchors = SkyrimAnchors::Get();
        const auto slot = anchors.singletonBSlot;

        a_os << std::format(
            "Task pool snapshot (Singleton-B @ {}+0x{:x}):\n",
            "module", anchors.singletonBSlotRVA);
        a_os << "\n";
        a_os << "  Skyrim's task-pool holder, identified by the Faster HDT-SMP-UP\n";
        a_os << "  maintainer (docs/case-study/27 §0). When main is parked inside\n";
        a_os << "  WaitForJobTask, this section reveals which layer of the chain\n";
        a_os << "  was torn down between the last healthy frame and the freeze\n";
        a_os << "  instant. The 'baseline' rows are sampled ≈1 Hz from the main\n";
        a_os << "  thread's Update hook; the 'frozen' rows are captured by the\n";
        a_os << "  watchdog at freeze time.\n";
        a_os << "\n";

        TaskPoolBaseline::Sample baseline{};
        const bool haveBaseline = TaskPoolBaseline::Latest(baseline);

        TaskPoolBaseline::Sample frozen{};
        if (!BuildFrozenSample(slot, frozen)) {
            a_os << "  <frozen-state capture faulted; nothing to compare>\n";
            return;
        }

        const auto nowMs = frozen.captureTickMs;
        a_os << std::format(
            "  Module base:                         0x{:016x}\n",
            anchors.moduleBase);
        a_os << std::format(
            "  WaitForJobTask RVA:                  0x{:x}\n",
            anchors.waitForJobTaskRVA);
        a_os << std::format(
            "  Singleton-B slot RVA:                0x{:x}\n",
            anchors.singletonBSlotRVA);
        if (haveBaseline) {
            const auto ageMs = (nowMs >= baseline.captureTickMs)
                                   ? (nowMs - baseline.captureTickMs)
                                   : 0;
            a_os << std::format(
                "  Last healthy baseline captured:      T-{}.{:03d} s before "
                "frozen capture\n",
                ageMs / 1000, ageMs % 1000);
            a_os << std::format(
                "                                       (baseline tick {} ms, "
                "frozen tick {} ms)\n",
                baseline.captureTickMs, nowMs);
        } else {
            a_os << "  Last healthy baseline captured:      <none — capture "
                    "ring is empty; this build either booted into a freeze\n"
                    "                                       within the first "
                    "second, or the baseline hook never armed>\n";
        }
        a_os << "\n";

        // ===== Layer 1: global slot =====================================
        a_os << "  ===== Layer 1: global slot =====\n";
        WriteDiffLine(
            a_os, 0,
            baseline.singletonPtr, frozen.singletonPtr,
            haveBaseline);
        a_os << "\n";

        // ===== Layer 2: singleton instance ==============================
        a_os << "  ===== Layer 2: singleton instance ("
             << "32 qwords) =====\n";
        if (frozen.singletonPtr == 0 &&
            (!haveBaseline || baseline.singletonPtr == 0))
        {
            a_os << "    <singleton ptr is null in both samples — task pool "
                    "not initialized>\n";
        } else {
            // Only diff when both samples reference the same instance.
            const bool layoutMatches = haveBaseline &&
                baseline.singletonPtr == frozen.singletonPtr;
            if (!layoutMatches && haveBaseline) {
                a_os << "    Note: baseline and frozen singleton pointers "
                        "differ; the instance was reallocated between\n"
                        "          samples, so per-offset diffing is not "
                        "meaningful. Showing frozen state only.\n";
            }
            WriteWindowDiff(
                a_os,
                baseline.singletonWindow,
                frozen.singletonWindow,
                TaskPoolBaseline::Sample::kSingletonWindow,
                layoutMatches);
        }
        a_os << "\n";

        // ===== Layer 3: sub_array =======================================
        a_os << "  ===== Layer 3: sub_array (16 qwords, indexed by arg1 of "
                "WaitForJobTask) =====\n";
        a_os << std::format(
            "    baseline sub_array ptr:  0x{:016x}\n",
            haveBaseline ? baseline.subArrayPtr : 0);
        a_os << std::format(
            "    frozen   sub_array ptr:  0x{:016x}{}\n",
            frozen.subArrayPtr,
            (haveBaseline &&
             baseline.subArrayPtr != frozen.subArrayPtr)
                ? "   <-- DIFF"
                : "");
        if (frozen.subArrayPtr == 0 &&
            (!haveBaseline || baseline.subArrayPtr == 0))
        {
            a_os << "    <both null — task pool has no jobs or is "
                    "uninitialized>\n";
        } else {
            const bool layoutMatches = haveBaseline &&
                baseline.subArrayPtr == frozen.subArrayPtr;
            WriteWindowDiff(
                a_os,
                baseline.subArrayWindow,
                frozen.subArrayWindow,
                TaskPoolBaseline::Sample::kSubArrayWidth,
                layoutMatches);
        }
        a_os << "\n";

        // ===== Layer 4: per-entry dispatch structs ======================
        a_os << "  ===== Layer 4: per-entry dispatch structs (each "
                "sub_array[i] -> 8 qwords) =====\n";
        if (frozen.populatedEntries == 0 && haveBaseline &&
            baseline.populatedEntries > 0)
        {
            a_os << "    Frozen sample has zero populated entries; baseline "
                    "had ";
            a_os << baseline.populatedEntries << ". Rendering baseline:\n";
            for (int i = 0; i < baseline.populatedEntries; ++i) {
                const auto& e = baseline.entries[i];
                a_os << std::format(
                    "    --- baseline entry [{:>2}] @ 0x{:016x} ---\n",
                    i, e.entryPtr);
                WriteWindowDiff(
                    a_os, e.entryWindow, e.entryWindow,
                    TaskPoolBaseline::Sample::kEntryWidth,
                    true);
                if (e.handleTablePtr != 0) {
                    a_os << std::format(
                        "        handle_table @ 0x{:016x}:\n",
                        e.handleTablePtr);
                    for (int j = 0;
                         j < TaskPoolBaseline::Sample::kHandleTableWidth;
                         ++j)
                    {
                        a_os << std::format(
                            "          [{:>1}] 0x{:016x}\n",
                            j, e.handleTableWindow[j]);
                    }
                }
            }
        } else if (frozen.populatedEntries == 0) {
            a_os << "    <no populated entries in either sample>\n";
        } else {
            for (int i = 0; i < frozen.populatedEntries; ++i) {
                const auto& fe = frozen.entries[i];
                const auto* be = haveBaseline
                                     ? FindBaselineEntryByPtr(baseline,
                                                              fe.entryPtr)
                                     : nullptr;
                a_os << std::format(
                    "    --- frozen entry [{:>2}] @ 0x{:016x}{} ---\n",
                    i, fe.entryPtr,
                    be ? "  (matched against baseline)"
                       : "  (no baseline match — new or reallocated)");
                WriteWindowDiff(
                    a_os,
                    be ? be->entryWindow : fe.entryWindow,
                    fe.entryWindow,
                    TaskPoolBaseline::Sample::kEntryWidth,
                    /*a_haveBaseline=*/be != nullptr);
                if (fe.handleTablePtr != 0 || (be && be->handleTablePtr != 0)) {
                    a_os << std::format(
                        "        handle_table @ baseline=0x{:016x} "
                        "frozen=0x{:016x}{}\n",
                        be ? be->handleTablePtr : 0,
                        fe.handleTablePtr,
                        (be && be->handleTablePtr != fe.handleTablePtr)
                            ? "   <-- DIFF"
                            : "");
                    for (int j = 0;
                         j < TaskPoolBaseline::Sample::kHandleTableWidth;
                         ++j)
                    {
                        const auto b = be ? be->handleTableWindow[j] : 0;
                        const auto f = fe.handleTableWindow[j];
                        a_os << std::format(
                            "          [{:>1}] baseline 0x{:016x}  "
                            "frozen 0x{:016x}{}\n",
                            j, b, f,
                            (be && b != f) ? "   <-- DIFF" : "");
                    }
                }
            }
        }
        a_os << "\n";

        // ===== Investigation hint =======================================
        a_os << "  Investigation hint:\n";
        a_os << "    Main's wait HANDLE at freeze time appears in the Threads\n";
        a_os << "    section as the value KERNELBASE clobbered into main's\n";
        a_os << "    RBX. Scan the Layer-4 baseline blocks above for that\n";
        a_os << "    same handle value — whichever entry's handle_table\n";
        a_os << "    contained it identifies the queue index that main was\n";
        a_os << "    waiting on. The producer that should have signaled it\n";
        a_os << "    lives somewhere in Skyrim's task pool — per the FSMP\n";
        a_os << "    maintainer (case-study 27 §0), the wait is unrelated\n";
        a_os << "    to hdtsmp64.dll even when an HDT-SMP frame is visible\n";
        a_os << "    above the wait in main's stack. The automated search\n";
        a_os << "    below does this cross-reference for you.\n";

        // ===== Automated producer attribution ===========================
        // Guarded by the surrounding Reporter::Section (SEH + C++). Suspends
        // each worker briefly to read its context — same pattern as the
        // Threads section, and safe at freeze time.
        WriteJobAttribution(a_os, frozen);
    }

}
