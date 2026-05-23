#include "PCH.h"
#include "Phase4Defer.h"
#include "Config.h"
#include "Stats.h"

namespace WorkerSpinLockFix::Phase4Defer {

    namespace {

        // ---------------------------------------------------------------
        // Diagnostic logging gate. Cached at Install() from
        // Config::Get().phase4_defer_diagnostic_logging so the hot path
        // does a single relaxed-load instead of going through the
        // full Config accessor.
        //
        // When true, every hook entry/exit emits a log line.
        // ---------------------------------------------------------------
        std::atomic<bool> g_diag_log{ false };

        inline bool DiagLogEnabled() noexcept {
            return g_diag_log.load(std::memory_order_relaxed);
        }

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
        // Layout constants for the synchronous half of the deferred path
        // (added in v2.0.1 to fix the skyshard / scripted-animation
        // activation regression -- see docs/case-study/24-v2-0-1-...md).
        //
        // kBoolBitsOffset: byte offset of Actor::boolBits within Actor.
        //   Per CommonLibSSE-NG (RE/A/Actor.h).
        //
        // kInTempChangeListMask: bit 9, == Actor::BOOL_BITS::kInTempChangeList.
        //   id 40333 sets this bit; id 40334 clears it. The flag is
        //   queried elsewhere in the engine to decide "is this actor
        //   currently being tracked in the temp change list?". v2.0.0
        //   deferred the bit toggle along with the bucket-array update,
        //   which made bit 9 reflect the OLD state for the duration of
        //   the deferral window. Some downstream code paths in id 36016
        //   read bit 9 to drive observable behaviour (most visibly:
        //   scripted activation animations like skyshards). v2.0.1
        //   splits the operation: the bit toggle happens synchronously
        //   in the gate (correctness), the bucket-array update is
        //   deferred (cycle break).
        //
        // The atomic op is idempotent w.r.t. the drain's later call to
        // the original (fetch_or sets-an-already-set bit to set;
        // fetch_and clears-an-already-clear bit to clear). The original
        // re-runs the bit op under LockB; both writes converge to the
        // same final value.
        // ---------------------------------------------------------------
        constexpr std::ptrdiff_t  kBoolBitsOffset       = 0xe0;
        constexpr std::uint32_t   kInTempChangeListMask = 0x200u;  // bit 9

        inline std::atomic<std::uint32_t>* BoolBitsAtomic(void* actor) noexcept {
            // Actor::boolBits is std::atomic<std::uint32_t> at +0xe0
            // (CommonLibSSE-NG RE/A/Actor.h). reinterpret_cast is safe:
            // std::atomic<std::uint32_t> is layout-compatible with a
            // plain uint32_t on all targets we ship to (x86-64 MSVC).
            return reinterpret_cast<std::atomic<std::uint32_t>*>(
                reinterpret_cast<std::byte*>(actor) + kBoolBitsOffset);
        }

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
            const bool diag = DiagLogEnabled();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] DRAIN tid={} count={}",
                    ::GetCurrentThreadId(),
                    local.size());
            }
            for (const auto& c : local) {
                if (c.kind == DeferKind::kAdd) {
                    g_hook_add.unsafe_call<void>(c.pl, c.actor);
                } else {
                    g_hook_remove.unsafe_call<void>(c.pl, c.actor);
                }
                Stats::OnPhase4Drained();
                if (diag) {
                    logs::info(
                        "[Phase4Defer.diag]   drained kind={} pl={} actor={}",
                        c.kind == DeferKind::kAdd ? "ADD" : "REMOVE",
                        c.pl, c.actor);
                }
            }
        }

        // ---------------------------------------------------------------
        // Wrap of id 19369.
        //
        // Calling convention - SIX args (NOT four):
        //
        //     bool __fastcall id19369(
        //         void*       rcx,         // arg 1, qword pointer
        //         void*       rdx,         // arg 2, qword pointer
        //         uint8_t     r8b,         // arg 3, BYTE (only low byte
        //                                  //              saved by prologue)
        //         uintptr_t   r9,          // arg 4, qword
        //         uint32_t    stack_arg5,  // arg 5, dword at [orig_rsp+0x28]
        //         uint8_t     stack_arg6); // arg 6, byte  at [orig_rsp+0x30]
        //
        // Evidence (from analysis/out_id19369_full.txt):
        //   prologue at +0x0..+0x12 sets up rbp = orig_rsp - 0x4f, so
        //   any access to [rbp + 0x77 .. + 0x7f] reads stack args 5
        //   and 6. The function reads them at:
        //
        //     +0x47   movzx  eax, byte ptr  [rbp + 0x7f]   ; arg 6 (byte)
        //     +0x87   mov    eax, dword ptr [rbp + 0x77]   ; arg 5 (dword)
        //     +0x2f5  cmp    byte ptr       [rbp + 0x7f], 0
        //     +0x41f  mov    r9d, dword ptr [rbp + 0x77]
        //     +0x583  mov    edx, dword ptr [rbp + 0x77]
        //     +0x5c1  mov    r9d, dword ptr [rbp + 0x77]
        //
        // and forwards both stack args verbatim through the recursive
        // self-call at +0x9d, so we have to as well. v2.0.0 and the
        // first cut of v2.0.1 declared the wrap with only 4 register
        // args. When the engine called id 19369 with 6 args, the
        // engine wrote the stack args at [orig_rsp+0x28] / +0x30, but
        // our hook ignored them and called the trampoline through a
        // 4-arg `unsafe_call`, which placed our (uninitialised) outgoing
        // stack-arg slots there instead. The trampoline then read
        // garbage for args 5 and 6. Args 5 and 6 happen to drive
        // observable behaviour for at least some callers, including
        // scripted-animation activators (skyshards being the most
        // visible case).
        //
        // Return type: `bool`. id 19369's epilogue is
        //
        //     +0x682  movzx  eax, bl
        //     +0x697  ret
        //
        // i.e. it returns a bool zero-extended from `bl` into `eax`.
        // Throughout the body `bl` is set deliberately (`mov ebx, 1`
        // on success branches, `sete bl` on conditionals) -- the value
        // is observed by callers to drive downstream behaviour.
        //
        // We capture the original's return via `unsafe_call<bool>`,
        // run the depth bookkeeping + drain, then propagate the
        // captured value. The depth counter is incremented BEFORE
        // the original runs so that id 40333 / id 40334 calls made
        // inside the original's body see tl_lockA_depth > 0 and
        // queue. It is decremented AFTER the original returns. The
        // drain runs outside the LockA scope (the original's
        // BSAutoLock destructor has already released LockA by the
        // time we reach the epilogue), so the deferred LockB
        // acquires happen with no AB-BA hazard.
        // ---------------------------------------------------------------
        bool __fastcall HookedLockAAcquirer(
            void*           rcx,
            void*           rdx,
            std::uint8_t    r8b,
            std::uintptr_t  r9,
            std::uint32_t   stack_arg5,
            std::uint8_t    stack_arg6)
        {
            const bool diag = DiagLogEnabled();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] LockA-WRAP-ENTER tid={} "
                    "rcx={} rdx={} r8b=0x{:02x} r9=0x{:x} "
                    "arg5=0x{:08x} arg6=0x{:02x} depth_in={}",
                    ::GetCurrentThreadId(),
                    rcx, rdx,
                    static_cast<unsigned>(r8b),
                    static_cast<std::uint64_t>(r9),
                    static_cast<unsigned>(stack_arg5),
                    static_cast<unsigned>(stack_arg6),
                    tl_lockA_depth);
            }

            ++tl_lockA_depth;

            const bool result = g_hook_lockA_acquirer.unsafe_call<bool>(
                rcx, rdx, r8b, r9, stack_arg5, stack_arg6);

            const int depth = --tl_lockA_depth;
            if (depth == 0) {
                DrainDeferredOnExit();
            }

            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] LockA-WRAP-EXIT  tid={} "
                    "result={} depth_out={}",
                    ::GetCurrentThreadId(),
                    result ? 1 : 0,
                    depth);
            }
            return result;
        }

        // ---------------------------------------------------------------
        // Gate on id 40333 (`AddToTempChangeList`). If LockA is held
        // by this thread, set kInTempChangeList synchronously (no
        // LockB needed -- it's a single atomic op on the actor),
        // queue (pl, actor) for the deferred bucket-array append,
        // and return. Otherwise tail-call the original.
        //
        // The split (synchronous bit, deferred array) was added in
        // v2.0.1 to fix the skyshard / scripted-animation activation
        // regression. v2.0.0 deferred the bit toggle as well; some
        // downstream paths in id 36016 read kInTempChangeList to
        // decide whether to fire an animation, and the deferred-bit
        // window made those reads see stale state.
        //
        // The drain (DrainDeferredOnExit) calls the original, which
        // re-sets the bit under LockB. Both writes converge to the
        // same final value (idempotent fetch_or).
        // ---------------------------------------------------------------
        void __fastcall HookedAddToTempChangeList(void* pl, void* actor) {
            const bool diag = DiagLogEnabled();
            if (tl_lockA_depth > 0) {
                if (actor) {
                    BoolBitsAtomic(actor)->fetch_or(
                        kInTempChangeListMask,
                        std::memory_order_acq_rel);
                }
                tl_deferred.push_back({ DeferKind::kAdd, pl, actor });
                Stats::OnPhase4Queued();
                if (diag) {
                    logs::info(
                        "[Phase4Defer.diag] ADD-DEFER     tid={} "
                        "pl={} actor={} depth={} qsize={}",
                        ::GetCurrentThreadId(),
                        pl, actor, tl_lockA_depth, tl_deferred.size());
                }
                return;
            }
            Stats::OnPhase4PassThrough();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] ADD-PASS      tid={} "
                    "pl={} actor={} (no LockA held)",
                    ::GetCurrentThreadId(), pl, actor);
            }
            g_hook_add.unsafe_call<void>(pl, actor);
        }

        // ---------------------------------------------------------------
        // Gate on id 40334 (`RemoveFromTempChangeList`). Same shape
        // as the add gate: synchronously clear kInTempChangeList,
        // defer the bucket-array removal.
        //
        // The original also clears the private global at 0x2f44db0
        // if it currently points to actor; that part stays in the
        // deferred half. Doc 20 §1 confirmed that global is private
        // to id 40285 + id 40334 (zero external readers across the
        // binary), so the deferred clear cannot stale-read.
        // ---------------------------------------------------------------
        void __fastcall HookedRemoveFromTempChangeList(void* pl, void* actor) {
            const bool diag = DiagLogEnabled();
            if (tl_lockA_depth > 0) {
                if (actor) {
                    BoolBitsAtomic(actor)->fetch_and(
                        ~kInTempChangeListMask,
                        std::memory_order_acq_rel);
                }
                tl_deferred.push_back({ DeferKind::kRemove, pl, actor });
                Stats::OnPhase4Queued();
                if (diag) {
                    logs::info(
                        "[Phase4Defer.diag] REMOVE-DEFER  tid={} "
                        "pl={} actor={} depth={} qsize={}",
                        ::GetCurrentThreadId(),
                        pl, actor, tl_lockA_depth, tl_deferred.size());
                }
                return;
            }
            Stats::OnPhase4PassThrough();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] REMOVE-PASS   tid={} "
                    "pl={} actor={} (no LockA held)",
                    ::GetCurrentThreadId(), pl, actor);
            }
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

        g_diag_log.store(
            Config::Get().phase4_defer_diagnostic_logging,
            std::memory_order_relaxed);

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
                "[Phase4Defer] structural fix armed (v2.0.1: 6-arg "
                "bool-returning wrap on id 19369 + synchronous "
                "kInTempChangeList toggle + deferred bucket-array op). "
                "Hooks: "
                "id 19369 (LockA acquirer wrap) at 0x{:x}, "
                "id 40333 (AddToTempChangeList gate) at 0x{:x}, "
                "id 40334 (RemoveFromTempChangeList gate) at 0x{:x}. "
                "LB->LA direction (id 40285 / id 36614 / id 38413) is "
                "intentionally not hooked. diagnostic_logging={}",
                a_lockA, a_add, a_remove,
                g_diag_log.load(std::memory_order_relaxed) ? "ON" : "OFF");
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
