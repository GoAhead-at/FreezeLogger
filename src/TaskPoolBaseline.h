#pragma once

#include <cstdint>

namespace FreezeLogger::TaskPoolBaseline {

    // Healthy-state snapshot of Skyrim's task-pool holder (Singleton-B
    // at SkyrimSE+0x2f26a70). Captured periodically from the main thread
    // while the game is running normally so that, at freeze time, the
    // watchdog can render the most recent healthy state side-by-side
    // with the (usually torn-down) frozen state.
    //
    // The disassembly target for the wait is:
    //
    //   SkyrimSE+0xc38130 (WaitForJobTask):
    //     rax = *(SkyrimSE+0x2f26a70)        ; singleton ptr
    //     rcx = [rax + 8]                    ; sub_array
    //     rcx = [rcx + arg1*8]               ; sub_array[arg1]
    //     rcx = [rcx]                        ; *sub_array[arg1] (handle table)
    //     rcx = [rcx + arg2*8]               ; handle table[arg2]
    //     jmp KERNEL32!WaitForSingleObject
    //
    // We capture each of those layers so the post-freeze report can show
    // which one was torn down. Caps are intentional: kSubArrayWidth=16
    // and kMaxEntries=8 — enough to cover every populated index seen
    // in the case-study corpus, with a constant memory footprint.
    struct Sample {
        constexpr static int kSingletonWindow = 32;
        constexpr static int kSubArrayWidth   = 16;
        constexpr static int kMaxEntries      = 8;
        constexpr static int kEntryWidth      = 8;
        constexpr static int kHandleTableWidth = 8;

        std::uint64_t  captureTickMs = 0;
        std::uintptr_t singletonPtr  = 0;
        std::uintptr_t subArrayPtr   = 0;

        std::uintptr_t singletonWindow[kSingletonWindow]{};
        std::uintptr_t subArrayWindow[kSubArrayWidth]{};

        // Per-entry capture for indices [0..kMaxEntries). entryPtr==0
        // means "this slot was null at capture time".
        struct Entry {
            std::uintptr_t entryPtr = 0;
            std::uintptr_t entryWindow[Sample::kEntryWidth]{};
            std::uintptr_t handleTablePtr = 0;
            std::uintptr_t handleTableWindow[Sample::kHandleTableWidth]{};
        };
        Entry entries[kMaxEntries]{};

        int populatedEntries = 0;       // count of entries with entryPtr != 0
    };

    // Resolves the SkyrimSE.exe base and arms periodic capture. Idempotent.
    // Safe to call before MainHook::Install.
    void Init();

    // Called from the Main::Update hook on every frame. Internally throttled
    // to capture at most once per ~kCaptureEveryNTicks frames (≈1 Hz at 60
    // fps), so the per-frame cost is just an atomic increment + compare on
    // 59 frames out of 60.
    void MaybeCapture() noexcept;

    // Copies out the most recently captured sample. Returns false if
    // no sample has been captured yet (e.g. freeze fired before the
    // first capture, or SkyrimSE base couldn't be resolved).
    // Thread-safe; serialized with the writer via a short mutex.
    bool Latest(Sample& a_out);

    // Test/observability seam.
    std::uint64_t LastCaptureTickMs();

}
