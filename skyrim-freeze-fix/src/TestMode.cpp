#include "PCH.h"
#include "TestMode.h"

#include "AcquireHook.h"
#include "Stats.h"
#include "WaitGraph.h"

namespace WorkerSpinLockFix::TestMode {

    namespace {

        // Heap-resident BSSpinLock-shaped objects. Allocated once at
        // first Run() and never freed; their lifetime extends past the
        // test threads, which is required because the AcquireHook hot
        // path may continue to read g_test_lockA / g_test_lockB even
        // after the test completes (they remain in the surgical filter
        // until cleared, and we clear them on success but a stuck thread
        // could still observe them).
        //
        // Aligned to a cache line. The engine's BSSpinLock is reportedly
        // 8 bytes (owner + state); we pad to 64 to keep the two test
        // locks on different cache lines so contention stays realistic.
        struct alignas(64) TestLock {
            std::uint32_t owner{ 0 };
            std::uint32_t state{ 0 };
            char          pad[64 - 8]{};
        };

        TestLock g_test_lockA;
        TestLock g_test_lockB;

        std::atomic<bool> g_running{ false };

        using AcquireFn = void(__fastcall*)(WaitGraph::Lock*);

        // Resolve the real BSSpinLock::Acquire (id 12210) entry-point
        // address. The hook intercepts calls to this address, so calling
        // through the resolved pointer routes through the surgical hook
        // exactly as the engine would. Returns nullptr on failure.
        AcquireFn ResolveAcquire() noexcept {
            try {
                const REL::Relocation<std::uintptr_t> acquire{ REL::ID(12210) };
                return reinterpret_cast<AcquireFn>(acquire.address());
            } catch (const std::exception& e) {
                logs::critical(
                    "[TEST] could not resolve BSSpinLock::Acquire (id 12210): {}",
                    e.what());
                return nullptr;
            }
        }

        // Worker A: holds lockA, then tries to acquire lockB. Sleeps
        // briefly between to give worker B time to also pick up its
        // first lock so the AB-BA window opens reliably.
        void WorkerA(AcquireFn acquire,
                     std::atomic<int>* started,
                     std::atomic<int>* completed)
        {
            started->fetch_add(1, std::memory_order_acq_rel);

            // Wait until both workers are ready before starting the
            // AB-BA dance, so the timing is symmetric.
            while (started->load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }

            const DWORD me = ::GetCurrentThreadId();
            logs::warn(
                "[TEST] worker A (TID {}) acquiring test_lockA "
                "({}).", me, static_cast<void*>(&g_test_lockA));
            acquire(reinterpret_cast<WaitGraph::Lock*>(&g_test_lockA));

            // Sleep enough that worker B has time to acquire test_lockB
            // before we go for the AB half. 25 ms is generous on any
            // realistic scheduler.
            std::this_thread::sleep_for(std::chrono::milliseconds(25));

            logs::warn(
                "[TEST] worker A (TID {}) acquiring test_lockB "
                "(this is the AB half; will spin until breaker fires).",
                me);
            acquire(reinterpret_cast<WaitGraph::Lock*>(&g_test_lockB));

            // We made it past the AB half. Release both locks manually.
            // The engine's release would normally just zero state and
            // leave owner intact; we mimic that.
            g_test_lockB.state = 0;
            g_test_lockA.state = 0;
            logs::warn(
                "[TEST] worker A (TID {}) completed; both test locks released.",
                me);
            completed->fetch_add(1, std::memory_order_acq_rel);
        }

        // Worker B: mirror of A. Holds lockB, then tries to acquire
        // lockA. The opposite order is what creates the AB-BA cycle
        // when both workers are mid-sequence.
        void WorkerB(AcquireFn acquire,
                     std::atomic<int>* started,
                     std::atomic<int>* completed)
        {
            started->fetch_add(1, std::memory_order_acq_rel);

            while (started->load(std::memory_order_acquire) < 2) {
                std::this_thread::yield();
            }

            const DWORD me = ::GetCurrentThreadId();
            logs::warn(
                "[TEST] worker B (TID {}) acquiring test_lockB "
                "({}).", me, static_cast<void*>(&g_test_lockB));
            acquire(reinterpret_cast<WaitGraph::Lock*>(&g_test_lockB));

            std::this_thread::sleep_for(std::chrono::milliseconds(25));

            logs::warn(
                "[TEST] worker B (TID {}) acquiring test_lockA "
                "(this is the BA half; will spin until breaker fires).",
                me);
            acquire(reinterpret_cast<WaitGraph::Lock*>(&g_test_lockA));

            g_test_lockA.state = 0;
            g_test_lockB.state = 0;
            logs::warn(
                "[TEST] worker B (TID {}) completed; both test locks released.",
                me);
            completed->fetch_add(1, std::memory_order_acq_rel);
        }

        // Coordinator: spawns the two workers, waits up to timeout_ms
        // for both to complete, then logs the verdict. If the timeout
        // elapses without completion, manually CASes both test locks'
        // state to 0 as a safety net so the test threads don't remain
        // spinning forever.
        void CoordinatorThread(std::uint32_t timeout_ms) {
            AcquireFn acquire = ResolveAcquire();
            if (acquire == nullptr) {
                logs::error("[TEST] FAILURE - could not resolve BSSpinLock::Acquire.");
                g_running.store(false, std::memory_order_release);
                return;
            }

            // Reset the test locks to a known-free state.
            g_test_lockA.owner = 0;
            g_test_lockA.state = 0;
            g_test_lockB.owner = 0;
            g_test_lockB.state = 0;

            // Register the test locks with the surgical filter so the
            // hook routes them through the slow path on contention.
            AcquireHook::AddTestLocks(
                reinterpret_cast<WaitGraph::Lock*>(&g_test_lockA),
                reinterpret_cast<WaitGraph::Lock*>(&g_test_lockB));

            logs::warn(
                "[TEST] starting synthetic AB-BA validation. "
                "test_lockA={}, test_lockB={}, timeout={}ms.",
                static_cast<void*>(&g_test_lockA),
                static_cast<void*>(&g_test_lockB),
                timeout_ms);

            std::atomic<int> started{ 0 };
            std::atomic<int> completed{ 0 };

            std::thread t_a(WorkerA, acquire, &started, &completed);
            std::thread t_b(WorkerB, acquire, &started, &completed);

            // Poll for completion or timeout.
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(timeout_ms);
            while (completed.load(std::memory_order_acquire) < 2 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            const int finished = completed.load(std::memory_order_acquire);
            if (finished == 2) {
                logs::warn(
                    "[TEST] SUCCESS - both workers completed. The breaker "
                    "detected the synthetic AB-BA, confirmed it via the "
                    "time-based flow, and force-released one test lock. "
                    "End-to-end cycle break is proven.");
            } else {
                logs::error(
                    "[TEST] FAILURE - {} of 2 workers completed before "
                    "timeout. The breaker did NOT fire. Manually clearing "
                    "test locks so the spinning workers can drain.",
                    finished);

                // Manual safety-net unstick: zero both state fields. The
                // engine's spin loop sees state=0 and acquires.
                ::InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(&g_test_lockA.state), 0);
                ::InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(&g_test_lockB.state), 0);

                // Wait a bit longer for the workers to drain after the
                // safety-net unstick. We do not detach because a leaked
                // thread reading our test-lock memory after we leave is
                // unsafe.
                const auto drain_deadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(5000);
                while (completed.load(std::memory_order_acquire) < 2 &&
                       std::chrono::steady_clock::now() < drain_deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }

            // Joining the workers is mandatory: they reference our test
            // locks. If we exit Coordinator before they finish, the
            // backing storage is fine (it's static) but the AcquireHook
            // is still routing them through the slow path. Joining
            // guarantees both workers have returned from acquire before
            // we clear the test slots.
            t_a.join();
            t_b.join();

            // We deliberately leave g_test_lockA / g_test_lockB
            // registered with AcquireHook even after the test ends.
            // They are static storage, never freed. Future engine
            // threads will compare-not-equal in the filter and bypass
            // immediately. Re-runs of the test reuse the same slots.

            g_running.store(false, std::memory_order_release);
        }

    } // namespace

    bool Run(std::uint32_t timeout_ms) {
        bool expected = false;
        if (!g_running.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            logs::warn("[TEST] Run() called while a test is already running; ignoring.");
            return false;
        }

        std::thread([timeout_ms]() {
            CoordinatorThread(timeout_ms);
        }).detach();
        return true;
    }

}
