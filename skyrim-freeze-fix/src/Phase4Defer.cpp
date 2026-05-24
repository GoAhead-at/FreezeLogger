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
        // When true, every wrap entry/exit and every gate fire emits
        // a log line.
        // ---------------------------------------------------------------
        std::atomic<bool> g_diag_log{ false };

        inline bool DiagLogEnabled() noexcept {
            return g_diag_log.load(std::memory_order_relaxed);
        }

        // ---------------------------------------------------------------
        // Address Library ids identified during Phase 4.1.
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
        // We never patch the function itself; we patch the SINGLE call
        // site that is reachable while LockA is held -- see below.
        constexpr std::uint64_t kID_AddToTempChangeList = 40333;

        // ProcessLists::RemoveFromTempChangeList(pl, actor). Locks
        // LockB, removes actor from the bucket array, clears bit 9 of
        // [actor+0xe0], clears the private global at 0x2f44db0 if it
        // points to actor. Same patching-strategy comment applies.
        constexpr std::uint64_t kID_RemoveFromTempChangeList = 40334;

        // The cycle hub: 96-way event-dispatch switch reached from
        // id 19369 -> id 19371 -> id 35974. Has a direct
        // `call id 40334` at +0xdcb (gated by a PlayerCharacter
        // pointer check at +0xdc4) and a direct `call id 19372` at
        // +0xfa3 (which then calls id 40333 internally).
        constexpr std::uint64_t   kID_CycleHub                 = 36016;
        constexpr std::ptrdiff_t  kOff_CycleHub_CallRemove     = 0xdcb;

        // The "AddToTempChangeList wrapper": small function reached
        // from the cycle hub at id 36016+0xfa3. Its body does some
        // stack-flag bookkeeping and then directly calls id 40333 at
        // +0x606. By patching id 19372+0x606 instead of patching the
        // entry of id 40333, we restrict our gate's blast radius to
        // exactly the cycle path -- callers of id 40333 from outside
        // the cycle do not pay the gate cost at all.
        constexpr std::uint64_t   kID_AddViaWrapper            = 19372;
        constexpr std::ptrdiff_t  kOff_AddViaWrapper_CallAdd   = 0x606;

        // ---------------------------------------------------------------
        // Per-thread state.
        //
        // tl_lockA_depth: how many id 19369 frames are open on this
        //   thread. Incremented in the wrap prologue, decremented in
        //   the wrap epilogue. Always 0 outside id 19369. Greater
        //   than 1 only on the (extremely rare) recursive case where
        //   id 19369 calls itself from within its own body. The
        //   thread-local counter handles the recursive case naturally.
        //
        // tl_deferred: queue of LockB-acquirer calls whose execution
        //   was postponed because LockA was held. Drained in FIFO
        //   order when tl_lockA_depth returns to 0. Lives in TLS so
        //   different threads' queues do not contend with each other
        //   and per-thread call ordering is preserved by construction.
        //
        // The TLS indirection costs one TLS load per call. Both
        // call sites we patch fire at low rate in normal play (a
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
        // Layout constants for the synchronous half of the deferred
        // path (added in v2.0.1 to fix the skyshard / scripted-
        // animation activation regression -- see
        // docs/case-study/24-v2-0-1-skyshard-regression-fix.md).
        //
        // kBoolBitsOffset:        byte offset of Actor::boolBits.
        // kInTempChangeListMask:  bit 9 == kInTempChangeList.
        //
        // The original id 40333 sets the bit and id 40334 clears it.
        // We mirror those writes synchronously in the gate so that
        // any reader who races against the deferred bucket-array
        // update still sees the correct flag value. The drain later
        // calls the original, which re-runs the bit op under LockB;
        // both writes converge to the same final value (idempotent
        // fetch_or / fetch_and on the same mask).
        // ---------------------------------------------------------------
        constexpr std::ptrdiff_t  kBoolBitsOffset       = 0xe0;
        constexpr std::uint32_t   kInTempChangeListMask = 0x200u;  // bit 9

        inline std::atomic<std::uint32_t>* BoolBitsAtomic(void* actor) noexcept {
            // Actor::boolBits is std::atomic<std::uint32_t> at +0xe0
            // (CommonLibSSE-NG RE/A/Actor.h). reinterpret_cast is safe:
            // std::atomic<std::uint32_t> is layout-compatible with a
            // plain uint32_t on x86-64 MSVC.
            return reinterpret_cast<std::atomic<std::uint32_t>*>(
                reinterpret_cast<std::byte*>(actor) + kBoolBitsOffset);
        }

        // ---------------------------------------------------------------
        // The function pointer signature shared by id 40333 and id
        // 40334. Both are __fastcall (rcx=ProcessLists*, rdx=Actor*),
        // return void.
        // ---------------------------------------------------------------
        using ProcessListsFn = void(__fastcall*)(void* pl, void* actor);

        // ---------------------------------------------------------------
        // Hook handles + call-back-to-original pointers.
        //
        // g_hook_lockA_acquirer is a safetyhook InlineHook on id 19369:
        //   a whole-function wrap is unavoidable here because we need
        //   to bracket every entry and exit of the LockA-acquirer to
        //   maintain tl_lockA_depth.
        //
        // For id 40333 / id 40334 we patch CALL SITES, not function
        // entries (a v2.0.1 architectural improvement informed by the
        // analysis of GarrixWong's skyrim-freeze-fix). g_orig_id40333
        // and g_orig_id40334 are plain function pointers to the
        // unmodified entry points; we use them in the drain (and in
        // the no-LockA-held passthrough branch) to invoke the original
        // function. Because we never patched the function entries,
        // any other mod that hooks id 40333 or id 40334 directly
        // continues to work for callers we do not intercept and
        // continues to run on top of the engine's code when we drain.
        // ---------------------------------------------------------------

        SafetyHookInline   g_hook_lockA_acquirer{};
        ProcessListsFn     g_orig_id40333{ nullptr };
        ProcessListsFn     g_orig_id40334{ nullptr };
        std::atomic<bool>  g_installed{ false };

        // ---------------------------------------------------------------
        // The drain. Called from the wrap epilogue when the thread's
        // LockA depth returns to 0 and there is queued work.
        //
        // We MOVE the queue out before iterating so that a deferred
        // call which itself reenters id 19369 (extremely unlikely)
        // can refill the queue without us re-iterating it. After
        // the move the local copy is iterated FIFO; per-thread call
        // ordering is preserved.
        // ---------------------------------------------------------------
        void DrainDeferredOnExit() {
            if (tl_deferred.empty()) {
                return;
            }
            std::vector<DeferredCall> local;
            local.swap(tl_deferred);
            const bool diag = DiagLogEnabled();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] DRAIN tid={} count={}",
                    ::GetCurrentThreadId(),
                    local.size());
            }
            for (const auto& c : local) {
                if (c.kind == DeferKind::kAdd) {
                    g_orig_id40333(c.pl, c.actor);
                } else {
                    g_orig_id40334(c.pl, c.actor);
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
        // Wrap of id 19369. Unchanged from v2.0.1's first cut; only
        // the surrounding gates have moved. See the long calling-
        // convention comment for why the signature is six args
        // returning bool (4 register + 2 stack args).
        //
        // Evidence (from analysis/out_id19369_full.txt):
        //   prologue at +0x0..+0x12 sets up rbp = orig_rsp - 0x4f, so
        //   any access to [rbp + 0x77 .. + 0x7f] reads stack args 5
        //   and 6. The function reads them at:
        //
        //     +0x47   movzx  eax, byte ptr  [rbp + 0x7f]   ; arg 6
        //     +0x87   mov    eax, dword ptr [rbp + 0x77]   ; arg 5
        //     +0x2f5  cmp    byte ptr       [rbp + 0x7f], 0
        //     +0x41f  mov    r9d, dword ptr [rbp + 0x77]
        //     +0x583  mov    edx, dword ptr [rbp + 0x77]
        //     +0x5c1  mov    r9d, dword ptr [rbp + 0x77]
        //
        // and forwards both stack args verbatim through the recursive
        // self-call at +0x9d, so we have to as well.
        //
        // Return value: id 19369 returns a bool zero-extended from
        // `bl` into `eax` at +0x682 (`movzx eax, bl; ret`). Callers
        // observe the value to drive downstream behaviour, including
        // scripted-animation activators. We capture and propagate it
        // via `unsafe_call<bool>`.
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
        // Call-site gate at id 36016+0xdcb (replaces the engine's
        // direct `call id 40334`). Fires only when execution reaches
        // this specific event-handler arm of the cycle hub.
        //
        // Calling convention matches id 40334:
        //   __fastcall RemoveFromTempChangeList(ProcessLists*, Actor*)
        //
        // If LockA is held by this thread, clear kInTempChangeList
        // synchronously and queue the bucket-array removal for the
        // drain. Otherwise tail-call the original.
        // ---------------------------------------------------------------
        void __fastcall HookedRemoveAtCycleHub(void* pl, void* actor) {
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
                        "[Phase4Defer.diag] REMOVE-DEFER  (id36016+0xdcb) "
                        "tid={} pl={} actor={} depth={} qsize={}",
                        ::GetCurrentThreadId(),
                        pl, actor, tl_lockA_depth, tl_deferred.size());
                }
                return;
            }
            Stats::OnPhase4PassThrough();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] REMOVE-PASS   (id36016+0xdcb) "
                    "tid={} pl={} actor={} (no LockA held)",
                    ::GetCurrentThreadId(), pl, actor);
            }
            g_orig_id40334(pl, actor);
        }

        // ---------------------------------------------------------------
        // Call-site gate at id 19372+0x606 (replaces the inner
        // `call id 40333` inside the AddToTempChangeList wrapper).
        // Same shape as the remove gate: synchronously set
        // kInTempChangeList, defer the bucket-array append.
        //
        // Reaching this gate implies one of:
        //   (a) the cycle hub fired its +0xfa3 arm (id 36016 -> id 19372);
        //   (b) some other caller invoked id 19372 directly.
        // Both cases are handled identically: defer iff LockA is held,
        // pass through otherwise. Case (b) is rare but harmless.
        // ---------------------------------------------------------------
        void __fastcall HookedAddInsideAddWrapper(void* pl, void* actor) {
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
                        "[Phase4Defer.diag] ADD-DEFER     (id19372+0x606) "
                        "tid={} pl={} actor={} depth={} qsize={}",
                        ::GetCurrentThreadId(),
                        pl, actor, tl_lockA_depth, tl_deferred.size());
                }
                return;
            }
            Stats::OnPhase4PassThrough();
            if (diag) {
                logs::info(
                    "[Phase4Defer.diag] ADD-PASS      (id19372+0x606) "
                    "tid={} pl={} actor={} (no LockA held)",
                    ::GetCurrentThreadId(), pl, actor);
            }
            g_orig_id40333(pl, actor);
        }

        // ---------------------------------------------------------------
        // Resolve an addrlib id to a runtime function address.
        // Returns 0 on failure with a critical log.
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

        // ---------------------------------------------------------------
        // Verify that `call_site` is a 5-byte CALL (0xE8 + rel32) and
        // that its rel32 target equals `expected_target`. Returns
        // true if the call site matches the engine's pristine layout
        // and is safe to patch. Returns false (with a critical log)
        // if the bytes do not match -- in that case the call has
        // probably been rewritten by another mod, and patching it
        // would silently stomp the other mod's hook.
        // ---------------------------------------------------------------
        bool VerifyCallSite(
            std::uintptr_t  call_site,
            std::uintptr_t  expected_target,
            const char*     what)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(call_site);
            if (bytes[0] != 0xE8) {
                logs::critical(
                    "[Phase4Defer] {} (0x{:x}) does not begin with E8 "
                    "(found 0x{:02x}). Aborting patch -- another mod "
                    "may already have rewritten this call site.",
                    what, call_site, static_cast<unsigned>(bytes[0]));
                return false;
            }
            const auto rel32 = *reinterpret_cast<const std::int32_t*>(bytes + 1);
            const auto actual_target = static_cast<std::uintptr_t>(
                static_cast<std::int64_t>(call_site) + 5 + rel32);
            if (actual_target != expected_target) {
                logs::critical(
                    "[Phase4Defer] {} (0x{:x}) targets 0x{:x} but we "
                    "expected 0x{:x}. Aborting patch -- another mod "
                    "may already have redirected this call site.",
                    what, call_site, actual_target, expected_target);
                return false;
            }
            return true;
        }

    } // namespace

    bool Install() {
        if (g_installed.exchange(true, std::memory_order_acq_rel)) {
            return true;
        }

        g_diag_log.store(
            Config::Get().phase4_defer_diagnostic_logging,
            std::memory_order_relaxed);

        const auto a_lockA       = ResolveId(kID_LockA_Acquirer,
                                             "LockA acquirer (id 19369)");
        const auto a_add         = ResolveId(kID_AddToTempChangeList,
                                             "AddToTempChangeList (id 40333)");
        const auto a_remove      = ResolveId(kID_RemoveFromTempChangeList,
                                             "RemoveFromTempChangeList (id 40334)");
        const auto a_cyclehub    = ResolveId(kID_CycleHub,
                                             "cycle hub (id 36016)");
        const auto a_addwrapper  = ResolveId(kID_AddViaWrapper,
                                             "Add wrapper (id 19372)");
        if (a_lockA == 0 || a_add == 0 || a_remove == 0 ||
            a_cyclehub == 0 || a_addwrapper == 0) {
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        const auto remove_call_site =
            a_cyclehub + static_cast<std::uintptr_t>(kOff_CycleHub_CallRemove);
        const auto add_call_site =
            a_addwrapper + static_cast<std::uintptr_t>(kOff_AddViaWrapper_CallAdd);

        if (!VerifyCallSite(remove_call_site, a_remove,
                            "id 36016+0xdcb (call id 40334)")) {
            g_installed.store(false, std::memory_order_release);
            return false;
        }
        if (!VerifyCallSite(add_call_site, a_add,
                            "id 19372+0x606 (call id 40333)")) {
            g_installed.store(false, std::memory_order_release);
            return false;
        }

        try {
            // Reserve queue space up-front so the hot path never has
            // to allocate. A single id 19369 frame typically queues
            // 0-3 calls; 8 is comfortably above the worst case.
            tl_deferred.reserve(8);

            // 1. Inline hook on the LockA acquirer -- this still
            //    needs a whole-function wrap because tl_lockA_depth
            //    must bracket every entry/exit.
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

            // 2. Save unmodified entry points of id 40333 / id 40334
            //    so the drain and the passthrough branches can call
            //    the originals directly.
            g_orig_id40333 = reinterpret_cast<ProcessListsFn>(a_add);
            g_orig_id40334 = reinterpret_cast<ProcessListsFn>(a_remove);

            // 3. Patch the two cycle-hub call sites via the SKSE
            //    trampoline. This rewrites only the 5-byte CALL
            //    instructions at id 36016+0xdcb and id 19372+0x606;
            //    the entry points of id 40333 and id 40334 remain
            //    pristine, so any other mod's hooks on those
            //    functions continue to run normally for callers we
            //    do not intercept.
            auto& trampoline = SKSE::GetTrampoline();
            trampoline.write_call<5>(
                remove_call_site,
                reinterpret_cast<std::uintptr_t>(&HookedRemoveAtCycleHub));
            trampoline.write_call<5>(
                add_call_site,
                reinterpret_cast<std::uintptr_t>(&HookedAddInsideAddWrapper));

            g_hook_lockA_acquirer = std::move(h_lockA);

            logs::info(
                "[Phase4Defer] structural fix armed (v2.0.1 call-site "
                "edition: 6-arg bool-returning wrap on id 19369 + two "
                "surgical call-site patches inside the cycle hub). "
                "Hooks: "
                "id 19369 (LockA acquirer wrap) at 0x{:x}, "
                "id 36016+0xdcb (-> HookedRemoveAtCycleHub) at 0x{:x}, "
                "id 19372+0x606 (-> HookedAddInsideAddWrapper) at 0x{:x}. "
                "Function entries of id 40333 / id 40334 are NOT patched, "
                "so other mods that hook those functions still work. "
                "LB->LA direction (id 40285 / id 36614 / id 38413) is "
                "intentionally not hooked. diagnostic_logging={}",
                a_lockA, remove_call_site, add_call_site,
                g_diag_log.load(std::memory_order_relaxed) ? "ON" : "OFF");
            return true;
        } catch (const std::exception& e) {
            logs::critical(
                "[Phase4Defer] install threw: {}. LockA wrap torn down. "
                "Note: any call-site patches that were already written "
                "before the throw are now pointing at handlers that the "
                "wrap-teardown leaves valid (the handlers are static "
                "functions in this DLL). Plugin will continue, but the "
                "Phase4Defer module is considered un-armed.",
                e.what());
            g_hook_lockA_acquirer = {};
            g_installed.store(false, std::memory_order_release);
            return false;
        }
    }

}
