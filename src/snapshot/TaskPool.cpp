#include "PCH.h"
#include "snapshot/TaskPool.h"

#include "TaskPoolBaseline.h"

#include <cstring>
#include <string>

namespace FreezeLogger::Snapshot::TaskPool {

    namespace {

        // Must stay in sync with TaskPoolBaseline / MainWaitProbe / Verdict.
        constexpr std::uintptr_t kSingletonBPtrRVA = 0x2f26a70;

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
            std::uintptr_t            a_base,
            TaskPoolBaseline::Sample& a_out) noexcept
        {
            a_out = TaskPoolBaseline::Sample{};
            a_out.captureTickMs = ::GetTickCount64();

            std::uintptr_t singleton = 0;
            if (!TryReadQword(a_base + kSingletonBPtrRVA, singleton)) {
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

    }   // anonymous namespace

    void Write(std::ostream& a_os) {
        const HMODULE skyrim = ::GetModuleHandleW(L"SkyrimSE.exe");
        if (!skyrim) {
            a_os << "<SkyrimSE.exe module handle unavailable; "
                    "task-pool snapshot skipped>\n";
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(skyrim);

        a_os << "Task pool snapshot (Singleton-B @ SkyrimSE+0x2f26a70):\n";
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
        if (!BuildFrozenSample(base, frozen)) {
            a_os << "  <frozen-state capture faulted; nothing to compare>\n";
            return;
        }

        const auto nowMs = frozen.captureTickMs;
        a_os << std::format(
            "  SkyrimSE base:                       0x{:016x}\n", base);
        a_os << std::format(
            "  Singleton-B RVA:                     0x{:x}\n",
            kSingletonBPtrRVA);
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
        a_os << "    above the wait in main's stack.\n";
    }

}
