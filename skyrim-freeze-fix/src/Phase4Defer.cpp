#include "PCH.h"
#include "Phase4Defer.h"
#include "Stats.h"

namespace WorkerSpinLockFix::Phase4Defer {

    namespace {

        // ---------------------------------------------------------------
        // Address Library ids the case study identified in Phase 4.1.
        // See docs/case-study/22-v2-phase4-1-cycle-hub-characterisation.md.
        // ---------------------------------------------------------------

        // The LockA acquirer. Acquires LockA on entry, releases on
        // return. The whole AB-BA cycle's LA->LB direction passes
        // through this function: id 19369 -> id 19371 -> id 35974 ->
        // id 36016 -> { id 40334 OR id 19372 -> id 40333 }.
        constexpr std::uint64_t kID_LockA_Acquirer = 19369;

        // ProcessLists::AddToTempChangeList(pl, actor). Locks LockB,
        // appends actor to the bucket array at [pl+0x158], sets bit 9
        // of [actor+0xe0] (Actor::BOOL_BITS::kInTempChangeList).
        constexpr std::uint64_t kID_AddToTempChangeList = 40333;

        // ProcessLists::RemoveFromTempChangeList(pl, actor). Locks
        // LockB, removes actor from the bucket array, clears bit 9 of
        // [actor+0xe0], clears the private global at 0x2f44db0 if it
        // points to actor.
        constexpr std::uint64_t kID_RemoveFromTempChangeList = 40334;

        // ---------------------------------------------------------------
        // Per-thread state.
        //
        // g_lockA_depth: how many id 19369 frames are open on this
        //   thread. Incremented in the wrap prologue, decremented in
        //   the wrap epilogue. Always 0 outside id 19369. Greater than
        //   1 only on the (extremely rare) recursive case where id
        //   19369 reentered itself from within its own body. The
        //   thread-local counter handles the recursive case naturally.
        //
        // g_deferred: queue of LockB acquirer calls whose execution
        //   was postponed because LockA was held. Drained in
        //   FIFO order when g_lockA_depth returns to 0. Lives in
        //   thread-local storage so different threads' queues do not
        //   contend with each other and per-thread call ordering is
        //   preserved by construction.
        //
        // The thread-local indirection costs one TLS load per call.
        // Both id 40333 and id 40334 are LOW-RATE in normal play (a
        // few hundred calls per second across all threads) so the
        // cost is negligible.
        // ---------------------------------------------------------------

        enum class DeferKind : std::uint8_t {
            kAdd,     // call original id 40333
            kRemove,  // call original id 40334
        };

        struct DeferredCall {
            DeferKind kind;
            void*     pl;     // ProcessLists*
            void*     actor;  // Actor*
        };

        thread_local int                       tl_lockA_depth{ 0 };
        thread_local std::vector<DeferredCall> tl_deferred{};

        // ---------------------------------------------------------------
        // Hook handles. These are written exactly once during Install()
        // and never modified after. Reads from the hot path (the wrap
        // and the gates) are safe because they happen strictly after
        // Install() has returned.
        // ---------------------------------------------------------------

        SafetyHookInline   g_hook_lockA_acquirer{};
        SafetyHookInline   g_hook_add{};
        SafetyHookInline   g_hook_remove{};
        std::atomic<bool>  g_installed{ false };

        // ---------------------------------------------------------------
        // The drain. Called from the wrap epilogue when the thread's
        // LockA depth returns to 0 and there is queued work.
        //
        // We MOVE the queue out before iterating so that a deferred
        // call which itself enters id 36016 -> id 40333 / id 40334
        // (extremely unlikely; would mean the engine called id 19369
        // recursively from within id 40333 / id 40334 -- it doesn't)
        // can refill the queue without us re-iterating it. After the
        // move the local copy is iterated FIFO; per-thread call
        // ordering is preserved.
        // ---------------------------------------------------------------
        void DrainDeferredOnExit() {
            if (tl_deferred.empty()) {
                return;
            }
            std::vector<DeferredCall> local;
            local.swap(tl_deferred);
            // tl_deferred is now empty; any nested deferral lands in
            // the fresh empty queue.
            for (const auto& c : local) {
                if (c.kind == DeferKind::kAdd) {
                    g_hook_add.unsafe_call<void>(c.pl, c.actor);
                } else {
                    g_hook_remove.unsafe_call<void>(c.pl, c.actor);
                }
                Stats::OnPhase4Drained();
            }
        }

        // ---------------------------------------------------------------
        // Wrap of id 19369. Calling convention matches id 19369's
        // observed prologue (`__fastcall` with 4 register args: 2
        // pointers, 1 byte, 1 qword). Stack args beyond rcx/rdx/r8/r9
        // are preserved transparently because we never touch the
        // stack at the call site -- we just forward to the trampoline
        // and pass the 4 register args through.
        //
        // The depth counter is incremented BEFORE the original runs
        // so that id 40333 / id 40334 calls made inside the original's
        // body see g_lockA_depth > 0 and queue. It is decremented
        // AFTER the original returns. The drain runs outside the
        // LockA scope (the original's BSAutoLock destructor has
        // already released LockA by the time we reach the epilogue),
        // so the deferred LockB acquires happen with no AB-BA hazard.
        // ---------------------------------------------------------------
        void __fastcall HookedLockAAcquirer(
            void*           rcx,
            void*           rdx,
            std::uint8_t    r8b,
            std::uintptr_t  r9)
        {
            ++tl_lockA_depth;

            g_hook_lockA_acquirer.unsafe_call<void>(rcx, rdx, r8b, r9);

            const int depth = --tl_lockA_depth;
            if (depth == 0) {
                DrainDeferredOnExit();
            }
        }

        // ---------------------------------------------------------------
        // Gate on id 40333 (`AddToTempChangeList`). If LockA is held
        // by this thread, queue (pl, actor) and return. Otherwise
        // tail-call the original.
        //
        // The case study's correctness audit (doc 22, section 2)
        // verified that the immediate post-call followup at the only
        // cycle-firing call site (id 19372 +0x606) reads only a
        // stack flag and does not observe id 40333's mutations. So
        // deferring is safe: the appended bucket-array entry and the
        // bit-9 set will both happen before the LockA wrap returns,
        // which is BEFORE id 19369's caller runs any state-reading
        // followup of its own.
        // ---------------------------------------------------------------
        void __fastcall HookedAddToTempChangeList(void* pl, void* actor) {
            if (tl_lockA_depth > 0) {
                tl_deferred.push_back({ DeferKind::kAdd, pl, actor });
                Stats::OnPhase4Queued();
                return;
            }
            Stats::OnPhase4PassThrough();
            g_hook_add.unsafe_call<void>(pl, actor);
        }

        // ---------------------------------------------------------------
        // Gate on id 40334 (`RemoveFromTempChangeList`). Same shape
        // as the add gate.
        //
        // Doc 22 audited the followup at id 36016 +0xdcb (the direct
        // call site): it reads bit 30 of [rbx+0xe0]. id 40334 writes
        // ONLY bit 9 of that field. Different bits; deferral is safe.
        // ---------------------------------------------------------------
        void __fastcall HookedRemoveFromTempChangeList(void* pl, void* actor) {
            if (tl_lockA_depth > 0) {
                tl_deferred.push_back({ DeferKind::kRemove, pl, actor });
                Stats::OnPhase4Queued();
                return;
            }
            Stats::OnPhase4PassThrough();
            g_hook_remove.unsafe_call<void>(pl, actor);
        }

        // ---------------------------------------------------------------
        // Resolve an addrlib id to a runtime function address. Returns
        // 0 on failure with a critical log. Idempotent across calls.
        // ---------------------------------------------------------------
        std::uintptr_t ResolveId(std::uint64_t id, const char* what) {
            try {
                const REL::Relocation<std::uintptr_t> rel{ REL::ID(id) };
                return rel.address();
            } catch (const std::exception& e) {
                logs::critical(
                    "[Phase4Defer] failed to resolve id {} ({}): {}",
                    id, what, e.what());
                return 0;
            }
        }

    } // namespace

    bool Install() {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        const auto a_lockA  = ResolveId(kID_LockA_Acquirer,
                                        "LockA acquirer");
        const auto a_add    = ResolveId(kID_AddToTempChangeList,
                                        "AddToTempChangeList");
        const auto a_remove = ResolveId(kID_RemoveFromTempChangeList,
                                        "RemoveFromTempChangeList");
        if (a_lockA == 0 || a_add == 0 || a_remove == 0) {
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        try {
            // Reserve queue space up-front so the hot path never has
            // to allocate. A single id 19369 frame typically queues
            // 0-3 calls; 8 is comfortably above the worst case.
            tl_deferred.reserve(8);

            auto h_lockA = safetyhook::create_inline(
                reinterpret_cast<void*>(a_lockA),
                reinterpret_cast<void*>(&HookedLockAAcquirer));
            if (!h_lockA) {
                logs::critical(
                    "[Phase4Defer] safetyhook::create_inline FAILED on "
                    "id 19369 at 0x{:x}; LockA wrap not installed.",
                    a_lockA);
                g_installed.store(false, std::memory_order_release);
                return false;
            }

            auto h_add = safetyhook::create_inline(
                reinterpret_cast<void*>(a_add),
                reinterpret_cast<void*>(&HookedAddToTempChangeList));
            if (!h_add) {
                logs::critical(
                    "[Phase4Defer] safetyhook::create_inline FAILED on "
                    "id 40333 at 0x{:x}; tearing down LockA wrap.",
                    a_add);
                h_lockA = {};  // explicit teardown of the LockA wrap
                g_installed.store(false, std::memory_order_release);
                return false;
            }

            auto h_remove = safetyhook::create_inline(
                reinterpret_cast<void*>(a_remove),
                reinterpret_cast<void*>(&HookedRemoveFromTempChangeList));
            if (!h_remove) {
                logs::critical(
                    "[Phase4Defer] safetyhook::create_inline FAILED on "
                    "id 40334 at 0x{:x}; tearing down LockA wrap and "
                    "id-40333 gate.", a_remove);
                h_lockA = {};
                h_add   = {};
                g_installed.store(false, std::memory_order_release);
                return false;
            }

            g_hook_lockA_acquirer = std::move(h_lockA);
            g_hook_add            = std::move(h_add);
            g_hook_remove         = std::move(h_remove);

            logs::info(
                "[Phase4Defer] structural fix armed. Hooks: "
                "id 19369 (LockA acquirer wrap) at 0x{:x}, "
                "id 40333 (AddToTempChangeList gate) at 0x{:x}, "
                "id 40334 (RemoveFromTempChangeList gate) at 0x{:x}. "
                "LB->LA direction (id 40285 / id 36614 / id 38413) is "
                "intentionally not hooked.",
                a_lockA, a_add, a_remove);
            return true;
        } catch (const std::exception& e) {
            logs::critical(
                "[Phase4Defer] install threw: {}. All hooks torn down.",
                e.what());
            g_hook_lockA_acquirer = {};
            g_hook_add            = {};
            g_hook_remove         = {};
            g_installed.store(false, std::memory_order_release);
            return false;
        }
    }

}
