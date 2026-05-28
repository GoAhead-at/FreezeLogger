#include "PCH.h"
#include "TaskPoolBaseline.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace FreezeLogger::TaskPoolBaseline {

    namespace {

        // Must stay in sync with MainWaitProbe / Verdict.
        constexpr std::uintptr_t kSingletonBPtrRVA = 0x2f26a70;

        // Frame throttling. 60 ticks ≈ 1 Hz at 60 fps. We want a recent
        // baseline at freeze time, but not so frequent that the cost is
        // noticeable on the main thread. The capture itself is a handful
        // of memory reads plus a brief mutex; well under 50 µs measured.
        constexpr std::uint32_t kCaptureEveryNTicks = 60;

        std::atomic<std::uintptr_t> g_skyrimBase{0};
        std::atomic<std::uint32_t>  g_tickCount{0};

        // The mutex is held only during the per-capture commit (memcpy
        // into g_latest) and the per-read copy out. Both holds are
        // sub-microsecond; we never hold the lock across SEH or DbgHelp.
        std::mutex                  g_mutex;
        Sample                      g_latest;
        bool                        g_haveSample = false;
        std::atomic<std::uint64_t>  g_lastCaptureTickMs{0};

        // ----- SEH-bounded primitives ----------------------------------
        // These live in their own functions so MaybeCapture / BuildSample
        // (the SEH callers) stay free of C++ objects with non-trivial
        // destructors — MSVC C2712 forbids mixing them.
        bool TryReadQword(std::uintptr_t a_addr, std::uintptr_t& a_out) noexcept {
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Read N qwords into a fixed-size buffer. Faults zero out the
        // remaining tail.
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

        // Pure-data builder. No C++ destructors used here so the SEH
        // inside TryReadQword does not run afoul of C2712. Returns true
        // iff at least the singleton pointer was readable (a sample with
        // a null singleton is not useful as a baseline).
        bool BuildSample(std::uintptr_t a_base, Sample& a_out) noexcept {
            a_out = Sample{};
            a_out.captureTickMs = ::GetTickCount64();

            std::uintptr_t singleton = 0;
            if (!TryReadQword(a_base + kSingletonBPtrRVA, singleton) ||
                singleton == 0)
            {
                return false;
            }
            a_out.singletonPtr = singleton;
            ReadWindow(singleton, a_out.singletonWindow, Sample::kSingletonWindow);

            std::uintptr_t subArray = 0;
            if (!TryReadQword(singleton + 8, subArray)) {
                subArray = 0;
            }
            a_out.subArrayPtr = subArray;
            if (subArray != 0) {
                ReadWindow(subArray, a_out.subArrayWindow, Sample::kSubArrayWidth);
            }

            int populated = 0;
            if (subArray != 0) {
                for (int i = 0; i < Sample::kMaxEntries; ++i) {
                    std::uintptr_t entry = 0;
                    if (!TryReadQword(
                            subArray + i * sizeof(std::uintptr_t), entry) ||
                        entry == 0)
                    {
                        continue;
                    }
                    auto& e = a_out.entries[populated];
                    e.entryPtr = entry;
                    ReadWindow(entry, e.entryWindow, Sample::kEntryWidth);

                    std::uintptr_t handleTable = 0;
                    if (TryReadQword(entry, handleTable) && handleTable != 0) {
                        e.handleTablePtr = handleTable;
                        ReadWindow(handleTable, e.handleTableWindow,
                                   Sample::kHandleTableWidth);
                    }
                    ++populated;
                }
            }
            a_out.populatedEntries = populated;
            return true;
        }

    }   // anonymous

    void Init() {
        if (g_skyrimBase.load(std::memory_order_relaxed) != 0) return;
        const HMODULE h = ::GetModuleHandleW(L"SkyrimSE.exe");
        if (h) {
            g_skyrimBase.store(
                reinterpret_cast<std::uintptr_t>(h),
                std::memory_order_relaxed);
            logs::info(
                "TaskPoolBaseline armed (SkyrimSE.exe base 0x{:x}, "
                "Singleton-B RVA 0x{:x}, capture every {} ticks).",
                reinterpret_cast<std::uintptr_t>(h),
                kSingletonBPtrRVA,
                kCaptureEveryNTicks);
        } else {
            logs::warn(
                "TaskPoolBaseline::Init could not resolve SkyrimSE.exe; "
                "baseline capture is disabled.");
        }
    }

    void MaybeCapture() noexcept {
        const auto base = g_skyrimBase.load(std::memory_order_relaxed);
        if (base == 0) return;

        // Tick gate first — on 59 out of 60 frames we touch nothing else.
        const auto tick = g_tickCount.fetch_add(1, std::memory_order_relaxed);
        if ((tick % kCaptureEveryNTicks) != 0) return;

        Sample sample;
        if (!BuildSample(base, sample)) return;

        // Commit. Holding the lock across a single memcpy is fine; the
        // watchdog reader's copy-out is the only contender and runs at
        // freeze time only.
        {
            std::scoped_lock lock(g_mutex);
            g_latest     = sample;
            g_haveSample = true;
        }
        g_lastCaptureTickMs.store(sample.captureTickMs,
                                  std::memory_order_relaxed);
    }

    bool Latest(Sample& a_out) {
        std::scoped_lock lock(g_mutex);
        if (!g_haveSample) return false;
        a_out = g_latest;
        return true;
    }

    std::uint64_t LastCaptureTickMs() {
        return g_lastCaptureTickMs.load(std::memory_order_relaxed);
    }

}
