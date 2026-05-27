#include "PCH.h"
#include "snapshot/MainWaitProbe.h"

#include "Heartbeat.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <vector>

namespace FreezeLogger::Snapshot::MainWaitProbe {

    namespace {

        // Address Library v1.5.97. The "lock primitive" function lives at
        // [SkyrimSE+0x5765d0, SkyrimSE+0x576620). Disassembled prologue:
        //
        //   0x5765d0  push rbx
        //   0x5765d2  sub  rsp, 0x20
        //   0x5765d6  mov  rbx, qword ptr [rip + 0x29b008b]   ; load singleton ptr
        //   0x5765dd  test rbx, rbx
        //   0x5765e0  je   0x140576606                         ; bail if null
        //   0x5765e2  cmp  dword ptr [rbx + 0x6c], 1           ; pending?
        //   0x5765e6  jne  0x140576606                         ; bail if not
        //   0x5765e8  mov  rcx, qword ptr [rbx + 0x60]         ; HANDLE -> rcx
        //   0x5765ec  xor  r8d, r8d                            ; bAlertable=FALSE
        //   0x5765ef  or   edx, 0xffffffff                     ; INFINITE
        //   0x5765f2  mov  dword ptr [rbx + 0x68], 0
        //   0x5765f9  call qword ptr [rip + 0xf92af1]          ; WaitForSingleObjectEx
        //   0x5765ff  mov  dword ptr [rbx + 0x6c], 0           ; clear pending
        //
        // RVA derivation: instruction at +0x5765d6 is 7 bytes; capstone
        // confirms the RIP-relative target resolves to 0x5765dd +
        // 0x29b008b = 0x2f26668. (Earlier comments said 0x2f266c8 — that
        // was a hand-math error; the real slot is 0x60 lower. The probes
        // before fix-up were reading random bytes at +singleton+0x48.)
        // The qword stored here is the singleton pointer that the lock
        // primitive loads into RBX.
        constexpr std::uintptr_t kSingletonPtrRVA = 0x2f26668;
        constexpr std::uintptr_t kLockFnLoRVA     = 0x5765d0;
        constexpr std::uintptr_t kLockFnHiRVA     = 0x576620;
        constexpr std::uintptr_t kLockReturnRVA   = 0x5765ff;

        // Second deadlock site discovered in freeze_2026-05-18_131625:
        // Main::Update has a SECOND infinite-wait call that does NOT route
        // through the +0x5765d0 lock primitive. The flow is:
        //
        //   SkyrimSE+0x5b34f9   call qword ptr [...] -> +0xc38130
        //   SkyrimSE+0x5b34fe   <-- main's saved return address
        //
        //   SkyrimSE+0xc38130   mov rax, [rip + 0x22ee539]   ; load Singleton-B
        //                       (target = SkyrimSE+0x2f26a70)
        //                       mov r8d, ecx                 ; arg1 -> r8d
        //                       mov rcx, [rax + 8]           ; sub-array
        //                       mov rcx, [rcx + r8*8]        ; sub-array[arg1]
        //                       test rcx, rcx
        //                       je  ret
        //                       mov rcx, [rcx]               ; *(sub-array[arg1])
        //                       mov eax, edx                 ; arg2 -> eax
        //                       or  edx, 0xffffffff          ; INFINITE
        //                       mov rcx, [rcx + rax*8]       ; vtable[arg2]
        //                       jmp [rip + 0x8d112e]         ; -> KERNEL32!
        //                                                      WaitForSingleObject
        //
        // From Main::Update the call site uses ecx=0, edx=1, so the wait
        // is on (Singleton-B->subArray[0])->vtable[1].
        constexpr std::uintptr_t kSingletonBPtrRVA = 0x2f26a70;
        constexpr std::uintptr_t kWaitWrapperLoRVA = 0xc38130;
        constexpr std::uintptr_t kWaitWrapperHiRVA = 0xc3815b;
        constexpr std::uintptr_t kMainUpdateRetBRVA = 0x5b34fe;

        // ----- ntdll!NtQueryEvent (read-only) ----------------------------
        // Used to inspect an event's state without consuming any signal.
        // WaitForSingleObject(h, 0) on an auto-reset event would steal
        // the signal from whoever scheduled it, so we never use it here.
        struct EVENT_BASIC_INFORMATION_ {
            LONG EventType;   // 0 = NotificationEvent (manual-reset)
                              // 1 = SynchronizationEvent (auto-reset)
            LONG EventState;  // 0 = not signaled, 1 = signaled
        };
        using NtQueryEventFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

        NtQueryEventFn LoadNtQueryEvent() noexcept {
            HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
            if (!ntdll) return nullptr;
            return reinterpret_cast<NtQueryEventFn>(
                ::GetProcAddress(ntdll, "NtQueryEvent"));
        }

        // ----- SEH-safe field readers -------------------------------------
        // CRITICAL: every function with __try/__except must avoid objects
        // with non-trivial destructors under /EHsc. These helpers pass
        // primitives by out-parameter so we can format the result outside
        // the SEH region.

        bool TryReadQword(std::uintptr_t a_addr, std::uintptr_t& a_out, DWORD& a_seh) noexcept {
            a_seh = 0;
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (a_seh = ::GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Singleton struct layout (SkyrimSE 1.5.97, derived from
        // disasm of id34547..id34563 cluster — see analysis/disasm_targets.txt):
        //
        //   [+0x58] HANDLE  worker-wake event   (main->worker, signaled by id34553)
        //   [+0x60] HANDLE  worker-ack  event   (worker->main, signaled by worker)
        //   [+0x68] uint32  work-id              (1 = id34553 path, 2 = id34552 path)
        //   [+0x6c] uint32  pending flag         (1 == wait scheduled and not yet drained)
        //   [+0x70] byte    flag2 (set by id34548/id34556)
        //   [+0x71] byte    flag3 (set by id34556/id34557)
        //   [+0x72] byte    flag4 (set by id34567/id34556)
        struct SingletonFields {
            std::uintptr_t worker_wake_handle;  // [+0x58]
            std::uintptr_t worker_ack_handle;   // [+0x60]
            std::uint32_t  work_id;             // [+0x68]
            std::uint32_t  pending;             // [+0x6c]
            std::uint8_t   flag2;               // [+0x70]
            std::uint8_t   flag3;               // [+0x71]
            std::uint8_t   flag4;               // [+0x72]
        };

        bool TryReadFields(
            std::uintptr_t   a_singleton,
            SingletonFields& a_out,
            DWORD&           a_seh) noexcept
        {
            a_seh = 0;
            __try {
                a_out.worker_wake_handle = *reinterpret_cast<volatile std::uintptr_t*>(a_singleton + 0x58);
                a_out.worker_ack_handle  = *reinterpret_cast<volatile std::uintptr_t*>(a_singleton + 0x60);
                a_out.work_id            = *reinterpret_cast<volatile std::uint32_t*>(a_singleton + 0x68);
                a_out.pending            = *reinterpret_cast<volatile std::uint32_t*>(a_singleton + 0x6c);
                a_out.flag2              = *reinterpret_cast<volatile std::uint8_t* >(a_singleton + 0x70);
                a_out.flag3              = *reinterpret_cast<volatile std::uint8_t* >(a_singleton + 0x71);
                a_out.flag4              = *reinterpret_cast<volatile std::uint8_t* >(a_singleton + 0x72);
                return true;
            } __except (a_seh = ::GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // Backward-compat helper for any future call sites that just want
        // the original three fields.
        bool TryReadFields(
            std::uintptr_t  a_singleton,
            std::uintptr_t& a_handle,
            std::uint32_t&  a_reserved,
            std::uint32_t&  a_pending,
            DWORD&          a_seh) noexcept
        {
            SingletonFields f{};
            const bool ok = TryReadFields(a_singleton, f, a_seh);
            a_handle   = f.worker_ack_handle;
            a_reserved = f.work_id;
            a_pending  = f.pending;
            return ok;
        }

        // Read a single qword. Used by the stack scan to test candidate
        // pointers. Stays as its own SEH frame so that scanning past
        // unmapped pages doesn't terminate the whole scan.
        bool TryReadHandleAt(
            std::uintptr_t  a_addr,
            std::uintptr_t& a_out,
            DWORD&          a_seh) noexcept
        {
            a_seh = 0;
            __try {
                a_out = *reinterpret_cast<volatile std::uintptr_t*>(a_addr);
                return true;
            } __except (a_seh = ::GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // ----- Main-thread context reader ---------------------------------
        // Suspends the main thread, captures its CONTEXT, resumes. The
        // singleton pointer is recovered from the non-volatile RBX register
        // (Windows x64 ABI guarantees RBX is callee-saved across the
        // WaitForSingleObjectEx call), even if the global storage at
        // SkyrimSE+kSingletonPtrRVA has since been overwritten by a racing
        // writer (which is exactly what freeze_2026-05-17_184345 showed:
        // global == 0x1 while main was still parked in the wait).
        //
        // POD-only locals so the function can stay noexcept and is safe to
        // mix with the SEH helpers above.
        struct MainCtxResult {
            bool           ok;
            DWORD          err;
            std::uintptr_t rip;
            std::uintptr_t rbx;
            std::uintptr_t rsp;
        };

        MainCtxResult ReadMainContext(DWORD a_mainTid) noexcept {
            MainCtxResult r{};
            if (a_mainTid == 0) {
                r.err = ERROR_INVALID_PARAMETER;
                return r;
            }
            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_mainTid);
            if (!h) {
                r.err = ::GetLastError();
                return r;
            }
            const DWORD prev = ::SuspendThread(h);
            if (prev == static_cast<DWORD>(-1)) {
                r.err = ::GetLastError();
                ::CloseHandle(h);
                return r;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (!::GetThreadContext(h, &ctx)) {
                r.err = ::GetLastError();
            } else {
                r.ok  = true;
                r.rip = static_cast<std::uintptr_t>(ctx.Rip);
                r.rbx = static_cast<std::uintptr_t>(ctx.Rbx);
                r.rsp = static_cast<std::uintptr_t>(ctx.Rsp);
            }
            ::ResumeThread(h);
            ::CloseHandle(h);
            return r;
        }

        // Scan the bottom of the stack (rsp .. rsp+limit) for a saved
        // return address that points back into the lock primitive's
        // post-wait RIP. Cheap, O(rsp_window/8), and unlike a full
        // StackWalk64 doesn't need DbgHelp's symbols mutex (so it never
        // deadlocks against the simultaneous Threads-section walk).
        //
        // Returns true iff the saved RIP at +0x5765ff was found, which
        // means main is *currently* parked inside our wait function.
        // Generic stack scanner for a single target qword. SEH-safe and
        // noexcept so callers can build std::strings around it without
        // tripping C2712.
        bool TrySawReturnAddr(
            std::uintptr_t a_rsp,
            std::uintptr_t a_target,
            std::size_t    a_windowBytes,
            std::size_t&   a_outDepthBytes,
            DWORD&         a_seh) noexcept
        {
            a_seh           = 0;
            a_outDepthBytes = 0;
            __try {
                for (std::size_t off = 0; off < a_windowBytes;
                     off += sizeof(std::uintptr_t))
                {
                    const auto v =
                        *reinterpret_cast<volatile std::uintptr_t*>(a_rsp + off);
                    if (v == a_target) {
                        a_outDepthBytes = off;
                        return true;
                    }
                }
                return false;
            } __except (a_seh = ::GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool TrySawWaitReturnAddr(
            std::uintptr_t a_rsp,
            std::uintptr_t a_skyrimBase,
            std::size_t&   a_outDepthBytes,
            DWORD&         a_seh) noexcept
        {
            return TrySawReturnAddr(
                a_rsp, a_skyrimBase + kLockReturnRVA, 0x400,
                a_outDepthBytes, a_seh);
        }

        const char* InterpretPending(std::uint32_t a_pending) {
            if (a_pending == 0) {
                return "no wait scheduled (cleared by lock primitive's post-wait writeback)";
            }
            if (a_pending == 1) {
                return "wait was scheduled — main is/was inside WaitForSingleObjectEx";
            }
            return "unexpected value (struct layout drift?)";
        }

        const char* EventTypeName(LONG a_type) {
            switch (a_type) {
            case 0:  return "NotificationEvent (manual-reset)";
            case 1:  return "SynchronizationEvent (auto-reset)";
            default: return "<unknown>";
            }
        }

        // Cheap test for "could this qword be a user-mode pointer?"
        // User-space x64 caps at 0x00007fff'ffffffff. Anything below the
        // first 64 KiB is the kernel's reserved low-page (always unmapped).
        bool LooksLikePointer(std::uintptr_t a_v) noexcept {
            return a_v >= 0x10000u && a_v <= 0x00007FFFFFFFFFFFu;
        }

        // Scan a stack window above rsp for a qword V such that V is a
        // plausible pointer AND *(V+0x60) == a_targetHandle. KERNELBASE's
        // prologue saves Skyrim's RBX (the singleton) somewhere in its
        // frame; the singleton's [+0x60] field holds the handle main is
        // waiting on; that handle is now visible in the live RBX register.
        // We use that to pin down the saved singleton slot without
        // needing a full StackWalk64.
        bool TryFindSingletonByHandle(
            std::uintptr_t  a_rsp,
            std::uintptr_t  a_targetHandle,
            std::uintptr_t& a_outSingleton,
            std::size_t&    a_outOffset) noexcept
        {
            a_outSingleton = 0;
            a_outOffset    = 0;
            constexpr std::size_t kWindow = 0x800;  // 2 KiB window
            for (std::size_t off = 0; off < kWindow; off += sizeof(std::uintptr_t)) {
                std::uintptr_t v   = 0;
                DWORD          seh = 0;
                if (!TryReadHandleAt(a_rsp + off, v, seh)) {
                    return false;  // hit unmapped page; further reads also fault
                }
                if (!LooksLikePointer(v)) continue;
                std::uintptr_t cand = 0;
                if (!TryReadHandleAt(v + 0x60, cand, seh)) continue;
                if (cand == a_targetHandle) {
                    a_outSingleton = v;
                    a_outOffset    = off;
                    return true;
                }
            }
            return false;
        }

        // Dump N qwords starting at rsp. Useful for off-line inspection
        // when the heuristics above don't find a match.
        void DumpStackWindow(
            std::ostream&  a_os,
            std::uintptr_t a_rsp,
            std::size_t    a_qwords)
        {
            for (std::size_t i = 0; i < a_qwords; ++i) {
                std::uintptr_t v   = 0;
                DWORD          seh = 0;
                const auto addr = a_rsp + i * sizeof(std::uintptr_t);
                if (!TryReadHandleAt(addr, v, seh)) {
                    a_os << std::format(
                        "      rsp+0x{:03x}  <fault SEH 0x{:08x}>\n",
                        i * sizeof(std::uintptr_t), seh);
                    return;
                }
                a_os << std::format(
                    "      rsp+0x{:03x}  0x{:016x}\n",
                    i * sizeof(std::uintptr_t), v);
            }
        }

        // Render a qword as up to 8 ASCII characters when its bytes are
        // printable. Returns the empty string when the qword does not
        // start with a plausible ASCII letter, so vtable / heap pointer
        // qwords are not noisily mis-rendered.
        //
        // The engine event-source-holder layout puts named event keys
        // ("Weekday", "Water", "Cast Magic Event", "Crime Gold Event")
        // inline within the singleton instance, so the existing hex
        // dump shows them as raw qwords. Annotating those qwords inline
        // saves the analyst from manually ASCII-decoding them.
        std::string DecodeQwordAsAscii(std::uintptr_t a_qword) noexcept {
            // Reject zero outright.
            if (a_qword == 0) return {};
            // First byte (low byte of the qword) must look like a
            // printable ASCII letter — anything else and we treat the
            // qword as non-text and skip the annotation.
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

        // Hex-dump arbitrary memory window. Used when we want to inspect a
        // singleton instance whose layout we don't fully understand yet —
        // the vtable pointer at offset 0 is the most-valuable single qword
        // since it ties the instance back to a class in SkyrimSE.exe.
        // Each qword that decodes as printable ASCII gets the decoded
        // string appended in quotes so engine event names are visible in
        // the dump (the Singleton-B event-source-holder layout exposes
        // these inline — see Appendix A of the case-study report).
        void DumpMemoryWindow(
            std::ostream&  a_os,
            std::uintptr_t a_addr,
            std::size_t    a_qwords)
        {
            for (std::size_t i = 0; i < a_qwords; ++i) {
                std::uintptr_t v   = 0;
                DWORD          seh = 0;
                const auto cur = a_addr + i * sizeof(std::uintptr_t);
                if (!TryReadHandleAt(cur, v, seh)) {
                    a_os << std::format(
                        "      +0x{:03x}  <fault SEH 0x{:08x}>\n",
                        i * sizeof(std::uintptr_t), seh);
                    return;
                }
                const auto ascii = DecodeQwordAsAscii(v);
                if (ascii.empty()) {
                    a_os << std::format(
                        "      +0x{:03x}  0x{:016x}\n",
                        i * sizeof(std::uintptr_t), v);
                } else {
                    a_os << std::format(
                        "      +0x{:03x}  0x{:016x}  \"{}\"\n",
                        i * sizeof(std::uintptr_t), v, ascii);
                }
            }
        }

        // Run NtQueryEvent on a candidate handle and write the result. Returns
        // true iff the handle was a valid kernel Event (so we got real
        // type/state values back). Used to validate that RBX really is a
        // kernel HANDLE (post-2026-05-17_202040 finding: KERNELBASE clobbers
        // the caller's RBX with the handle for its own internal use).
        bool QueryAndPrintEvent(
            std::ostream&  a_os,
            HANDLE         a_handle,
            std::int32_t&  a_outEventState,
            std::int32_t&  a_outEventType)
        {
            a_outEventState = -1;
            a_outEventType  = -1;
            static const auto pNtQueryEvent = LoadNtQueryEvent();
            if (!pNtQueryEvent) {
                a_os << "    <NtQueryEvent unavailable>\n";
                return false;
            }
            EVENT_BASIC_INFORMATION_ info{};
            ULONG returned = 0;
            const auto status = pNtQueryEvent(a_handle, /*EventBasicInformation=*/0,
                                              &info, sizeof(info), &returned);
            if (status != 0) {
                a_os << std::format(
                    "    NtQueryEvent NTSTATUS:               0x{:08x}",
                    static_cast<std::uint32_t>(status));
                if (static_cast<std::uint32_t>(status) == 0xC0000008u) {
                    a_os << "  (STATUS_INVALID_HANDLE)";
                } else if (static_cast<std::uint32_t>(status) == 0xC0000024u) {
                    a_os << "  (STATUS_OBJECT_TYPE_MISMATCH — "
                            "valid handle, but not an Event)";
                }
                a_os << "\n";
                return false;
            }
            a_outEventType  = info.EventType;
            a_outEventState = info.EventState;
            a_os << std::format(
                "    Event type:                          {} ({})\n",
                info.EventType, EventTypeName(info.EventType));
            a_os << std::format(
                "    Event state:                         {} ({})\n",
                info.EventState,
                info.EventState ? "SIGNALED" : "NOT signaled");
            return true;
        }

        // Run the full singleton-pointer interpretation: read [+0x60/+0x68/
        // +0x6c], NtQueryEvent the handle, print the deadlock verdict.
        // Used when we located the singleton via the stack scan, OR as a
        // fallback when RBX isn't a valid handle (older interpretation:
        // RBX is the singleton itself).
        void ProbeFromSingleton(
            std::ostream&       a_os,
            std::uintptr_t      a_singleton,
            std::string_view    a_label)
        {
            if (a_singleton == 0) {
                a_os << "  " << a_label << " is null — cannot probe.\n";
                return;
            }
            std::uintptr_t handleVal = 0;
            std::uint32_t  reserved  = 0;
            std::uint32_t  pending   = 0;
            DWORD          seh       = 0;
            if (!TryReadFields(a_singleton, handleVal, reserved, pending, seh)) {
                a_os << std::format(
                    "  <{} fields load faulted: SEH 0x{:08x} (singleton ptr "
                    "may be stale or struct layout changed)>\n",
                    a_label, seh);
                return;
            }
            a_os << std::format(
                "    [+0x60] event handle:                0x{:016x}\n", handleVal);
            a_os << std::format(
                "    [+0x68] reserved/work-id:            0x{:08x} ({})\n",
                reserved, reserved);
            a_os << std::format(
                "    [+0x6c] pending flag:                0x{:08x} ({})\n",
                pending, pending);
            a_os << "            interpretation:              "
                 << InterpretPending(pending) << "\n";

            const auto handle = reinterpret_cast<HANDLE>(handleVal);
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
                a_os << "  <event handle is null; nothing to query>\n";
                return;
            }
            std::int32_t evState = -1, evType = -1;
            QueryAndPrintEvent(a_os, handle, evState, evType);

            if (pending == 1 && evState == 0) {
                a_os << "\n";
                a_os << "    ===> DEADLOCK SIGNATURE MATCH <===\n";
                a_os << "         pending=1 means a wait was scheduled.\n";
                a_os << "         EventState=0 means the producer never signaled.\n";
                a_os << "         The main thread is parked, waiting for a Skyrim\n";
                a_os << "         worker that did not (yet) finish its task.\n";
            } else if (pending == 1 && evState == 1) {
                a_os << "\n";
                a_os << "    Note: pending=1 + EventState=1 means the producer did\n";
                a_os << "    signal but the main thread has not yet returned from\n";
                a_os << "    WaitForSingleObjectEx, or the auto-reset has not yet\n";
                a_os << "    consumed the signal. Less suspicious.\n";
            }
        }

    }

        // Snapshot of one suspended thread's TID + every integer register
        // we care about. We need RBX to find the parked worker (KERNELBASE
        // stashes the wait-handle there); RDI to read the BSSpinLock
        // pointer; RAX/RCX/RDX/RSI/R8..R15 to scan for any register
        // pointing into a known singleton (helps identify "the thread
        // currently touching Singleton-B" when none of them are parked
        // on the main wait handle).
        struct ThreadProbe {
            DWORD          tid;
            std::uintptr_t rip;
            std::uintptr_t rsp;
            std::uintptr_t rax;
            std::uintptr_t rbx;
            std::uintptr_t rcx;
            std::uintptr_t rdx;
            std::uintptr_t rsi;
            std::uintptr_t rdi;
            std::uintptr_t r8;
            std::uintptr_t r9;
            std::uintptr_t r10;
            std::uintptr_t r11;
            std::uintptr_t r12;
            std::uintptr_t r13;
            std::uintptr_t r14;
            std::uintptr_t r15;
        };

        std::vector<DWORD> EnumerateThreadsLocal(DWORD a_pid) noexcept {
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

        // For each thread other than self+main, briefly suspend, capture
        // RBX, resume. Looking for ones whose RBX matches the worker-wake
        // event handle. RAII would be cleaner but we need to stay free of
        // C++ destructors inside SEH frames; this function is plain noexcept.
        struct SpinLockRead {
            std::uint32_t owner;   // [+0x00]
            std::uint32_t state;   // [+0x04]
            DWORD         seh_owner;
            DWORD         seh_state;
        };

        SpinLockRead TryReadSpinLock(std::uintptr_t a_lockPtr) noexcept {
            SpinLockRead r{};
            r.owner = 0xffffffff;
            r.state = 0xffffffff;
            __try {
                r.owner = *reinterpret_cast<volatile std::uint32_t*>(a_lockPtr + 0);
            } __except (r.seh_owner = ::GetExceptionCode(),
                        EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                r.state = *reinterpret_cast<volatile std::uint32_t*>(a_lockPtr + 4);
            } __except (r.seh_state = ::GetExceptionCode(),
                        EXCEPTION_EXECUTE_HANDLER) {}
            return r;
        }

        ThreadProbe SnapshotThread(DWORD a_tid) noexcept {
            ThreadProbe p{};
            p.tid = a_tid;
            const HANDLE h = ::OpenThread(
                THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                    THREAD_QUERY_LIMITED_INFORMATION,
                FALSE, a_tid);
            if (!h) return p;
            const DWORD prev = ::SuspendThread(h);
            if (prev == static_cast<DWORD>(-1)) {
                ::CloseHandle(h);
                return p;
            }
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
            if (::GetThreadContext(h, &ctx)) {
                p.rip = static_cast<std::uintptr_t>(ctx.Rip);
                p.rsp = static_cast<std::uintptr_t>(ctx.Rsp);
                p.rax = static_cast<std::uintptr_t>(ctx.Rax);
                p.rbx = static_cast<std::uintptr_t>(ctx.Rbx);
                p.rcx = static_cast<std::uintptr_t>(ctx.Rcx);
                p.rdx = static_cast<std::uintptr_t>(ctx.Rdx);
                p.rsi = static_cast<std::uintptr_t>(ctx.Rsi);
                p.rdi = static_cast<std::uintptr_t>(ctx.Rdi);
                p.r8  = static_cast<std::uintptr_t>(ctx.R8);
                p.r9  = static_cast<std::uintptr_t>(ctx.R9);
                p.r10 = static_cast<std::uintptr_t>(ctx.R10);
                p.r11 = static_cast<std::uintptr_t>(ctx.R11);
                p.r12 = static_cast<std::uintptr_t>(ctx.R12);
                p.r13 = static_cast<std::uintptr_t>(ctx.R13);
                p.r14 = static_cast<std::uintptr_t>(ctx.R14);
                p.r15 = static_cast<std::uintptr_t>(ctx.R15);
            }
            ::ResumeThread(h);
            ::CloseHandle(h);
            return p;
        }

        // Returns a comma-separated list of register names whose value
        // equals a_target, or an empty string if none match. Skips RSP
        // (would always hit on stack-pivot stalls).
        std::string FindRegisterMatches(
            const ThreadProbe& a_p,
            std::uintptr_t     a_target)
        {
            if (a_target == 0) return {};
            std::string out;
            const struct { const char* name; std::uintptr_t v; } regs[] = {
                {"RAX", a_p.rax}, {"RBX", a_p.rbx}, {"RCX", a_p.rcx},
                {"RDX", a_p.rdx}, {"RSI", a_p.rsi}, {"RDI", a_p.rdi},
                {"R8",  a_p.r8 }, {"R9",  a_p.r9 }, {"R10", a_p.r10},
                {"R11", a_p.r11}, {"R12", a_p.r12}, {"R13", a_p.r13},
                {"R14", a_p.r14}, {"R15", a_p.r15},
            };
            for (const auto& r : regs) {
                if (r.v == a_target) {
                    if (!out.empty()) out += ",";
                    out += r.name;
                }
            }
            return out;
        }

        // ----- Singleton-B probe (the +0xc38130 path discovered 5/18) ----
        // Layout (best understood, from the constructor cluster id5578-id5600
        // around RVA +0x9220b..+0x92993; see analysis/xref_scan.txt):
        //
        //   *(SkyrimSE+0x2f26a70) -> ptr to a struct
        //     [+0x08] -> array of element-pointers (size unknown; only
        //                index 0 is observed used from Main::Update)
        //
        //   For each element[i]:
        //     [+0x00] -> "vtable_or_handles"
        //     vtable_or_handles[arg2*8]: HANDLE (when arg2=1, the value
        //                                returned to KERNEL32!WaitForSingleObject)
        //
        // We don't (yet) know the full struct width, so this is a
        // best-effort probe: it walks one chain (idx0=0, idx1=1) the same
        // way Main::Update does, prints every step, and queries the
        // resulting kernel HANDLE.
        struct SingletonBChain {
            bool           ok;
            DWORD          seh_step;            // first failing step (0 = none)
            std::uintptr_t global_ptr;          // *(base + kSingletonBPtrRVA)
            std::uintptr_t sub_array;           // *(global_ptr + 8)
            std::uintptr_t element0;            // *(sub_array + 0)
            std::uintptr_t element_vtable;      // *element0
            std::uintptr_t handle_at_idx1;      // element_vtable[1*8]
        };

        SingletonBChain WalkSingletonB(std::uintptr_t a_base) noexcept {
            SingletonBChain c{};
            DWORD seh = 0;
            if (!TryReadQword(a_base + kSingletonBPtrRVA, c.global_ptr, seh)) {
                c.seh_step = seh ? seh : 0xC0000005;
                return c;
            }
            if (c.global_ptr == 0) return c;
            if (!TryReadQword(c.global_ptr + 8, c.sub_array, seh)) {
                c.seh_step = seh ? seh : 0xC0000005;
                return c;
            }
            if (c.sub_array == 0) return c;
            if (!TryReadQword(c.sub_array + 0, c.element0, seh)) {
                c.seh_step = seh ? seh : 0xC0000005;
                return c;
            }
            if (c.element0 == 0) return c;
            if (!TryReadQword(c.element0 + 0, c.element_vtable, seh)) {
                c.seh_step = seh ? seh : 0xC0000005;
                return c;
            }
            if (c.element_vtable == 0) return c;
            if (!TryReadQword(c.element_vtable + 8, c.handle_at_idx1, seh)) {
                c.seh_step = seh ? seh : 0xC0000005;
                return c;
            }
            c.ok = true;
            return c;
        }

        // ----- Heuristic BSSpinLock candidate detector -------------------
        // BSSpinLock layout (from disasm of +0x132bd0):
        //   struct BSSpinLock { uint32_t threadID; uint32_t lockState; };
        //
        // When a worker is parked inside SleepEx waiting to retry the lock
        // acquire, RDI was supposed to still hold the lock pointer (it is
        // non-volatile in x64 ABI), but freeze_2026-05-19_110220 showed
        // RDI=0 for all four spinners — SleepEx's KERNELBASE implementation
        // evidently uses RDI as a working register before the syscall.
        //
        // So we fall back to scanning every register and the saved-context
        // stack window of the suspended thread for qwords that "look like"
        // a BSSpinLock pointer:
        //   * the qword V itself is a plausible user-space pointer
        //   * *(V+0) is small enough to be a TID (under 100k)
        //   * *(V+4) is 0 (free), 1 (held), or 2 (held with waiters)
        // and rank by number of fields that match.
        struct LockCandidate {
            std::uintptr_t addr;
            std::uint32_t  owner;
            std::uint32_t  state;
            const char*    src;     // "RDI", "stack+0xNN", etc.
        };

        bool LooksLikeLock(
            std::uintptr_t  a_v,
            std::uint32_t&  a_outOwner,
            std::uint32_t&  a_outState) noexcept
        {
            if (!LooksLikePointer(a_v)) return false;
            if ((a_v & 0x3) != 0) return false;  // 4-byte aligned at minimum
            std::uintptr_t pair = 0;
            DWORD seh = 0;
            if (!TryReadQword(a_v, pair, seh)) return false;
            const auto owner = static_cast<std::uint32_t>(pair & 0xffffffff);
            const auto state = static_cast<std::uint32_t>(pair >> 32);
            // owner is either 0 (free) or a plausible TID (Windows TIDs
            // never exceed ~100k in practice, and BSSpinLock stores the
            // raw GetCurrentThreadId() value).
            const bool ownerOk = owner == 0 || owner < 200000;
            // BSSpinLock state is binary in the disasm we have (0=free,
            // 1=held); allow 2 as headroom in case a future-proof bumped
            // counter ever lands here.
            const bool stateOk = state <= 2;
            if (!ownerOk || !stateOk) return false;
            a_outOwner = owner;
            a_outState = state;
            return true;
        }

        void CollectLockCandidates(
            const CONTEXT&              a_ctx,
            std::vector<LockCandidate>& a_out)
        {
            const struct { const char* name; std::uintptr_t v; } regs[] = {
                {"RAX", static_cast<std::uintptr_t>(a_ctx.Rax)},
                {"RBX", static_cast<std::uintptr_t>(a_ctx.Rbx)},
                {"RCX", static_cast<std::uintptr_t>(a_ctx.Rcx)},
                {"RDX", static_cast<std::uintptr_t>(a_ctx.Rdx)},
                {"RSI", static_cast<std::uintptr_t>(a_ctx.Rsi)},
                {"RDI", static_cast<std::uintptr_t>(a_ctx.Rdi)},
                {"R8",  static_cast<std::uintptr_t>(a_ctx.R8 )},
                {"R9",  static_cast<std::uintptr_t>(a_ctx.R9 )},
                {"R10", static_cast<std::uintptr_t>(a_ctx.R10)},
                {"R11", static_cast<std::uintptr_t>(a_ctx.R11)},
                {"R12", static_cast<std::uintptr_t>(a_ctx.R12)},
                {"R13", static_cast<std::uintptr_t>(a_ctx.R13)},
                {"R14", static_cast<std::uintptr_t>(a_ctx.R14)},
                {"R15", static_cast<std::uintptr_t>(a_ctx.R15)},
            };
            for (const auto& r : regs) {
                std::uint32_t owner = 0, state = 0;
                if (LooksLikeLock(r.v, owner, state)) {
                    a_out.push_back({r.v, owner, state, r.name});
                }
            }
            // Scan a 1KiB stack window for stack-saved lock pointers.
            // SleepEx's prologue saves the caller's RDI somewhere on its
            // stack; we don't know the exact offset, so we test every
            // qword.
            for (std::uintptr_t off = 0; off < 0x400; off += 8) {
                std::uintptr_t v   = 0;
                DWORD          seh = 0;
                if (!TryReadQword(static_cast<std::uintptr_t>(a_ctx.Rsp) + off,
                                  v, seh)) {
                    break;
                }
                std::uint32_t owner = 0, state = 0;
                if (LooksLikeLock(v, owner, state)) {
                    a_out.push_back({v, owner, state, "stack"});
                }
            }
        }

        // ----- BSSpinLock-owner search (extracted; runs unconditionally) -
        // Empirical pattern from freeze_2026-05-18_112604: BSThreadEvent
        // workers parked inside SkyrimSE+0x132bd0 (BSSpinLock::Acquire)
        // with the spin-retry return address SkyrimSE+0x132c5a on stack.
        // Lock layout: { uint32_t threadID; uint32_t lockState; }.
        void WriteSpinlockOwners(
            std::ostream&  a_os,
            std::uintptr_t a_base,
            DWORD          a_mainTid)
        {
            a_os << "\n  BSSpinLock-owner search "
                    "(threads spinning at SkyrimSE+0x132c5a):\n";
            constexpr std::uintptr_t kSpinRetRVA = 0x132c5a;
            const std::uintptr_t spinRetAddr = a_base + kSpinRetRVA;

            const DWORD pid     = ::GetCurrentProcessId();
            const DWORD selfTid = ::GetCurrentThreadId();
            const auto  tids    = EnumerateThreadsLocal(pid);

            int spinners = 0;
            for (DWORD tid : tids) {
                if (tid == selfTid) continue;
                const HANDLE h = ::OpenThread(
                    THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                        THREAD_QUERY_LIMITED_INFORMATION,
                    FALSE, tid);
                if (!h) continue;
                const DWORD prev = ::SuspendThread(h);
                if (prev == static_cast<DWORD>(-1)) {
                    ::CloseHandle(h);
                    continue;
                }
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;
                bool gotCtx = ::GetThreadContext(h, &ctx) != FALSE;
                ::ResumeThread(h);
                ::CloseHandle(h);
                if (!gotCtx) continue;

                bool isSpinning = false;
                for (std::uintptr_t off = 0; off < 0x100; off += 8) {
                    std::uintptr_t v = 0;
                    DWORD seh2 = 0;
                    if (!TryReadQword(static_cast<std::uintptr_t>(ctx.Rsp) + off,
                                      v, seh2)) {
                        break;
                    }
                    if (v == spinRetAddr) {
                        isSpinning = true;
                        break;
                    }
                }
                if (!isSpinning) continue;

                ++spinners;
                std::vector<LockCandidate> cands;
                CollectLockCandidates(ctx, cands);

                a_os << std::format("    TID {:>5}:", tid);
                if (cands.empty()) {
                    a_os << "  <no plausible BSSpinLock found in regs/stack>";
                } else {
                    // Print each unique (addr) candidate. Multiple regs
                    // can hold the same lock — coalesce.
                    std::vector<LockCandidate> uniq;
                    for (const auto& c : cands) {
                        bool dup = false;
                        for (const auto& u : uniq) {
                            if (u.addr == c.addr) { dup = true; break; }
                        }
                        if (!dup) uniq.push_back(c);
                    }
                    for (const auto& u : uniq) {
                        a_os << std::format(
                            "  [{}=0x{:016x} owner={} state={}",
                            u.src, u.addr, u.owner, u.state);
                        if (u.owner == a_mainTid) a_os << " <== MAIN!";
                        a_os << "]";
                    }
                }
                a_os << "\n";
            }
            if (spinners == 0) {
                a_os << "    <no thread is currently spinning on a BSSpinLock>\n";
            } else {
                a_os << std::format(
                    "    Total spinning threads: {}\n", spinners);
            }
        }

    void Write(std::ostream& a_os) {
        a_os << "Main::Update wait-helper probe (Skyrim SE 1.5.97):\n";
        a_os << "  Two known infinite-wait sites inside RE::Main::Update:\n";
        a_os << "    A) +0x5b35dd -> SkyrimSE+0x5765d0 (id 34554)\n";
        a_os << "         Singleton-A @ SkyrimSE+0x2f26668; reads HANDLE\n";
        a_os << "         from [singleton+0x60]; calls WaitForSingleObjectEx\n";
        a_os << "         (signature: pending=1 + ack-event NOT signaled).\n";
        a_os << "    B) +0x5b34fe -> SkyrimSE+0xc38130 (small wrapper)\n";
        a_os << "         Singleton-B @ SkyrimSE+0x2f26a70; walks\n";
        a_os << "         (*S)[+8][idx0]->vtable[idx1]; tail-jumps to\n";
        a_os << "         KERNEL32!WaitForSingleObject. Main::Update calls\n";
        a_os << "         this with idx0=0, idx1=1, dwMilliseconds=INFINITE.\n";
        a_os << "  KERNELBASE clobbers the caller's RBX with the HANDLE\n";
        a_os << "  for both wait functions, so we can read main's RBX and\n";
        a_os << "  treat it as the kernel HANDLE main is currently parked on.\n\n";

        const HMODULE skyrim = ::GetModuleHandleW(L"SkyrimSE.exe");
        if (!skyrim) {
            a_os << "  <SkyrimSE.exe module handle unavailable>\n";
            return;
        }
        const auto base = reinterpret_cast<std::uintptr_t>(skyrim);

        // ===== Probe 1: global slot at SkyrimSE+0x2f266c8 ================
        // Important caveat: a racing writer can clobber this slot while
        // main is parked. freeze_2026-05-17_184345 saw 0x1 here. We still
        // dump it — knowing what stomped on it is its own signal.
        const auto globalAddr = base + kSingletonPtrRVA;
        a_os << std::format(
            "  SkyrimSE base:                       0x{:016x}\n", base);
        a_os << std::format(
            "  Singleton ptr global address:        0x{:016x}\n", globalAddr);

        std::uintptr_t globalSingleton = 0;
        DWORD seh = 0;
        if (TryReadQword(globalAddr, globalSingleton, seh)) {
            a_os << std::format(
                "  Singleton ptr global value:          0x{:016x}\n",
                globalSingleton);
        } else {
            a_os << std::format(
                "  <global load faulted: SEH 0x{:08x}>\n", seh);
            globalSingleton = 0;
        }

        // ===== Probe 2: main thread's CONTEXT register snapshot ==========
        const auto mainTid = static_cast<DWORD>(Heartbeat::MainTid());
        a_os << "\n";
        a_os << "  Main thread context probe:\n";
        if (mainTid == 0) {
            a_os << "    <main TID unknown — heartbeat hasn't ticked yet>\n";
            return;
        }
        const auto m = ReadMainContext(mainTid);
        if (!m.ok) {
            a_os << std::format(
                "    <ReadMainContext failed: tid={}, err=0x{:08x}>\n",
                mainTid, m.err);
            return;
        }
        a_os << std::format(
            "    Main TID:                          {}\n", mainTid);
        a_os << std::format(
            "    Main RIP:                          0x{:016x}\n", m.rip);
        a_os << std::format(
            "    Main RBX:                          0x{:016x}\n", m.rbx);
        a_os << std::format(
            "    Main RSP:                          0x{:016x}\n", m.rsp);

        // ===== Wait-site identification ==================================
        // Two known infinite-wait return-into-Main::Update sites:
        //   A) +0x5765ff  (returns from the +0x5765d0 lock primitive)
        //   B) +0x5b34fe  (returns from the +0xc38130 wrapper that
        //                  tail-jumps to KERNEL32!WaitForSingleObject)
        // We detect both by scanning a small window above main's RSP.
        const bool ripInLockFnA =
            m.rip >= base + kLockFnLoRVA && m.rip < base + kLockFnHiRVA;
        const bool ripInWrapperB =
            m.rip >= base + kWaitWrapperLoRVA && m.rip < base + kWaitWrapperHiRVA;

        std::size_t retDepthA = 0;
        DWORD       scanSehA  = 0;
        const bool retAInStack = TrySawWaitReturnAddr(m.rsp, base, retDepthA, scanSehA);

        std::size_t retDepthB = 0;
        DWORD       scanSehB  = 0;
        const bool  retBInStack = TrySawReturnAddr(
            m.rsp, base + kMainUpdateRetBRVA, 0x400, retDepthB, scanSehB);

        const bool inSiteA = ripInLockFnA  || retAInStack;
        const bool inSiteB = ripInWrapperB || retBInStack;

        a_os << "    Wait-site detection:\n";
        if (inSiteA) {
            a_os << std::format(
                "      A) +0x5765d0 lock primitive: HIT  (rip-in-fn={}, "
                "return@rsp+0x{:x})\n",
                ripInLockFnA, retDepthA);
        } else {
            a_os << "      A) +0x5765d0 lock primitive: miss\n";
        }
        if (inSiteB) {
            a_os << std::format(
                "      B) +0xc38130 wrapper:        HIT  (rip-in-fn={}, "
                "return@rsp+0x{:x})\n",
                ripInWrapperB, retDepthB);
        } else {
            a_os << "      B) +0xc38130 wrapper:        miss\n";
        }
        if (scanSehA != 0 || scanSehB != 0) {
            a_os << std::format(
                "      (stack-scan SEH: A=0x{:08x}, B=0x{:08x})\n",
                scanSehA, scanSehB);
        }

        // ===== Probe 3: interpret RBX as the kernel HANDLE ===============
        // KERNELBASE!WaitForSingleObject{,Ex} both stash the caller's
        // handle parameter in RBX during the syscall (validated empirically
        // in freeze_..._202040 where RBX = 0x2a50 — clearly a handle, not
        // a Skyrim heap ptr). True for both wait-site A and B.
        const auto rbxHandle = reinterpret_cast<HANDLE>(m.rbx);
        a_os << "\n  Probe via RBX-as-HANDLE (KERNELBASE working register):\n";

        DWORD hflags = 0;
        const bool isHandle = ::GetHandleInformation(rbxHandle, &hflags) != FALSE;
        std::int32_t evState = -1, evType = -1;
        bool         isEvent = false;
        if (!isHandle) {
            const auto ge = ::GetLastError();
            a_os << std::format(
                "    GetHandleInformation:                FAILED (err 0x{:08x})\n",
                ge);
            a_os << "    RBX is NOT a current process handle — main may not\n"
                    "    be parked in a kernel wait (or the handle has\n"
                    "    been closed in a teardown race).\n";
        } else {
            a_os << std::format(
                "    GetHandleInformation flags:          0x{:08x}\n", hflags);
            isEvent = QueryAndPrintEvent(a_os, rbxHandle, evState, evType);
        }

        // ===== Site A: Singleton-A interpretation (id 34554 lock primitive)
        // Only meaningful when main is in/just-returned-from +0x5765d0.
        SingletonFields fields{};
        bool fieldsOk = false;
        if (inSiteA) {
            a_os << "\n  Site-A probe (Singleton-A @ SkyrimSE+0x2f26680):\n";
            a_os << "    Saved-singleton stack scan (looking for V where "
                    "*(V+0x60) == handle):\n";
            std::uintptr_t savedSingleton = 0;
            std::size_t    savedOffset    = 0;
            if (!TryFindSingletonByHandle(m.rsp, m.rbx, savedSingleton, savedOffset)) {
                a_os << "      <not found in 2 KiB window above rsp>\n";
                a_os << "      First 16 qwords above rsp (for off-line analysis):\n";
                DumpStackWindow(a_os, m.rsp, 16);
            } else {
                a_os << std::format(
                    "      Singleton found:                   0x{:016x} (at rsp+0x{:x})\n",
                    savedSingleton, savedOffset);

                DWORD fseh = 0;
                if (!TryReadFields(savedSingleton, fields, fseh)) {
                    a_os << std::format(
                        "      <singleton fields load faulted: SEH 0x{:08x}>\n", fseh);
                } else {
                    fieldsOk = true;
                    a_os << "    Singleton fields (full readback):\n";
                    a_os << std::format(
                        "      [+0x58] worker-wake handle:        0x{:016x}\n",
                        fields.worker_wake_handle);
                    a_os << std::format(
                        "      [+0x60] worker-ack  handle (echo): 0x{:016x}\n",
                        fields.worker_ack_handle);
                    a_os << std::format(
                        "      [+0x68] work-id:                   0x{:08x} ({})\n",
                        fields.work_id, fields.work_id);
                    a_os << std::format(
                        "      [+0x6c] pending flag:              0x{:08x} ({})\n",
                        fields.pending, fields.pending);
                    a_os << std::format(
                        "      [+0x70] flag2:                     0x{:02x}\n", fields.flag2);
                    a_os << std::format(
                        "      [+0x71] flag3:                     0x{:02x}\n", fields.flag3);
                    a_os << std::format(
                        "      [+0x72] flag4:                     0x{:02x}\n", fields.flag4);
                    a_os << "      interpretation: pending - "
                         << InterpretPending(fields.pending) << "\n";

                    a_os << "    Worker-wake event state ([+0x58] kernel event):\n";
                    const auto wakeHandle =
                        reinterpret_cast<HANDLE>(fields.worker_wake_handle);
                    if (wakeHandle == nullptr || wakeHandle == INVALID_HANDLE_VALUE) {
                        a_os << "      <worker-wake handle is null>\n";
                    } else {
                        DWORD wkflags = 0;
                        if (!::GetHandleInformation(wakeHandle, &wkflags)) {
                            a_os << std::format(
                                "      GetHandleInformation FAILED (err 0x{:08x})\n",
                                ::GetLastError());
                        } else {
                            std::int32_t wkState = -1, wkType = -1;
                            QueryAndPrintEvent(a_os, wakeHandle, wkState, wkType);
                        }
                    }

                    a_os << "    Worker-thread search (RBX == worker-wake handle):\n";
                    if (fields.worker_wake_handle == 0) {
                        a_os << "      <worker-wake handle is null; cannot search>\n";
                    } else {
                        const DWORD pid     = ::GetCurrentProcessId();
                        const DWORD selfTid = ::GetCurrentThreadId();
                        const auto  tids    = EnumerateThreadsLocal(pid);
                        int matches = 0;
                        for (DWORD tid : tids) {
                            if (tid == selfTid || tid == mainTid) continue;
                            const auto p = SnapshotThread(tid);
                            if (p.rbx == fields.worker_wake_handle) {
                                a_os << std::format(
                                    "      *** MATCH: TID {} (RIP 0x{:016x}, "
                                    "RBX 0x{:016x})\n",
                                    p.tid, p.rip, p.rbx);
                                ++matches;
                            }
                        }
                        if (matches == 0) {
                            a_os << "      <no other thread has RBX matching the "
                                    "worker-wake handle>\n";
                            a_os << "      Worker is NOT in WaitForSingleObjectEx — "
                                    "it may be running its job (check Threads / "
                                    "BSSpinLock probes below).\n";
                        } else {
                            a_os << "      Worker IS parked waiting for a wake "
                                    "signal — investigate why the wake never came.\n";
                        }
                    }
                }
            }
        }

        // ===== Site B: Singleton-B interpretation (id ?? — +0xc38130) =====
        // We don't yet know the full struct width nor the producer for this
        // wait, so the probe is exploratory. It walks the chain Main::Update
        // walks (idx0=0, idx1=1) and prints every step. The rbx-as-handle
        // value should match handle_at_idx1 — that's the strongest
        // confirmation we have today.
        SingletonBChain bChain{};
        bool bChainOk = false;
        if (inSiteB) {
            a_os << "\n  Site-B probe (Singleton-B @ SkyrimSE+0x2f26a70):\n";
            bChain = WalkSingletonB(base);
            a_os << std::format(
                "    *(SkyrimSE+0x2f26a70):               0x{:016x}\n",
                bChain.global_ptr);
            a_os << std::format(
                "    sub-array @ +0x08:                   0x{:016x}\n",
                bChain.sub_array);
            a_os << std::format(
                "    element[0] (sub_array+0):            0x{:016x}\n",
                bChain.element0);
            a_os << std::format(
                "    *element[0] (vtable_or_handles):     0x{:016x}\n",
                bChain.element_vtable);
            a_os << std::format(
                "    handle @ vtable[1]:                  0x{:016x}\n",
                bChain.handle_at_idx1);
            if (bChain.seh_step != 0) {
                a_os << std::format(
                    "    <chain walk faulted at step: SEH 0x{:08x}>\n",
                    bChain.seh_step);
            } else if (bChain.ok) {
                bChainOk = true;
                if (bChain.handle_at_idx1 == m.rbx) {
                    a_os << "    ===> handle_at_idx1 == main RBX — "
                            "chain confirmed.\n";
                } else {
                    a_os << "    Note: handle_at_idx1 != main RBX — main may be\n"
                            "          waiting on a different (idx0,idx1) tuple,\n"
                            "          or the chain is racing with a writer.\n";
                }
            } else if (bChain.global_ptr != 0) {
                // Chain walk completed but produced a null link (most
                // commonly: sub_array @ +8 was 0). When main is in the
                // wait, the structure MUST have been valid at the time
                // it was loaded, so a null link now means a writer
                // cleared it during the wait — interesting on its own.
                a_os << "    Note: chain walk produced a null link. The struct\n"
                        "          was valid when main loaded it (otherwise the\n"
                        "          wrapper would have early-returned without\n"
                        "          waiting), so SOMEONE cleared it after main\n"
                        "          went to sleep. See the singleton hex dump\n"
                        "          and the toucher-thread search below.\n";
            }

            // Hex-dump the singleton instance (vtable @ +0 is the most
            // useful single qword — points into SkyrimSE.exe's .rdata
            // and identifies the C++ class).
            if (bChain.global_ptr != 0) {
                a_os << std::format(
                    "    Singleton instance hex (32 qwords from "
                    "0x{:016x}):\n", bChain.global_ptr);
                DumpMemoryWindow(a_os, bChain.global_ptr, 32);
            }

            // Find any thread whose register set holds main's wait handle
            // (would be a co-consumer of the same event) or the singleton
            // instance pointer (a thread currently mutating the
            // singleton state — likely the producer, or someone racing
            // with it).
            const DWORD pid     = ::GetCurrentProcessId();
            const DWORD selfTid = ::GetCurrentThreadId();
            const auto  tids    = EnumerateThreadsLocal(pid);

            a_os << std::format(
                "    Co-consumer search (other thread with any register == "
                "main RBX 0x{:x}):\n", m.rbx);
            int handleHits = 0;
            for (DWORD tid : tids) {
                if (tid == selfTid || tid == mainTid) continue;
                const auto p = SnapshotThread(tid);
                const auto regs = FindRegisterMatches(p, m.rbx);
                if (!regs.empty()) {
                    a_os << std::format(
                        "      *** TID {:>5} (RIP 0x{:016x}): {} == handle\n",
                        p.tid, p.rip, regs);
                    ++handleHits;
                }
            }
            if (handleHits == 0) {
                a_os << "      <no other thread has main's wait handle in "
                        "any register>\n";
            }

            if (bChain.global_ptr != 0) {
                a_os << std::format(
                    "    Toucher-thread search (other thread with any register "
                    "== singleton 0x{:x}):\n", bChain.global_ptr);
                int touchHits = 0;
                for (DWORD tid : tids) {
                    if (tid == selfTid || tid == mainTid) continue;
                    const auto p = SnapshotThread(tid);
                    const auto regs = FindRegisterMatches(p, bChain.global_ptr);
                    if (!regs.empty()) {
                        a_os << std::format(
                            "      *** TID {:>5} (RIP 0x{:016x}): {} == "
                            "singleton\n",
                            p.tid, p.rip, regs);
                        ++touchHits;
                    }
                }
                if (touchHits == 0) {
                    a_os << "      <no other thread is currently touching "
                            "the singleton instance>\n";
                }
            }

            // Dump main's stack window (above RSP) so the off-line
            // analyst can read the saved arg1/arg2 pairs main passed
            // to +0xc38130.
            a_os << "    Main stack (32 qwords above RSP):\n";
            DumpStackWindow(a_os, m.rsp, 32);
        }

        // ===== Always-on: BSSpinLock-owner search ========================
        // Runs regardless of which wait site main is at. From the
        // 5/18 11:26 freeze, this exposed the lock-inversion deadlock
        // (4 worker threads spinning on a lock the main thread held).
        WriteSpinlockOwners(a_os, base, mainTid);

        // ===== Final verdict =============================================
        if (inSiteA && fieldsOk && fields.pending == 1 && evState == 0) {
            a_os << "\n";
            a_os << "    ===> DEADLOCK SIGNATURE MATCH <===\n";
            a_os << "         pending=1: id34553 set the work-pending flag.\n";
            a_os << "         worker-ack EventState=0: worker never signaled\n";
            a_os << "                                  completion to main.\n";
            a_os << "         Main is parked in id34554's INFINITE wait at\n";
            a_os << "         SkyrimSE+0x5765f9 → KERNELBASE!WaitForSingleObjectEx.\n";
            a_os << "         Investigation path: see worker-thread search and\n";
            a_os << "         BSSpinLock-owner search above. If the lock owner\n";
            a_os << "         is the main TID, this is a lock-inversion deadlock.\n";
        } else if (inSiteA && fieldsOk && fields.pending == 1 && evState == 1) {
            a_os << "\n";
            a_os << "    Note: pending=1 + worker-ack signaled — wait should\n";
            a_os << "    be unblocking imminently; may be a transient probe.\n";
        } else if (inSiteB) {
            a_os << "\n";
            a_os << "    Verdict: main is parked at the +0xc38130 wrapper\n";
            a_os << "    (Site B), waiting INFINITE on a HANDLE drawn from\n";
            a_os << "    Singleton-B. Producer mapping not yet decoded —\n";
            a_os << "    the constructor cluster id5578-id5600 (RVA\n";
            a_os << "    +0x9220b..+0x92993) is the next investigation\n";
            a_os << "    target. Cross-reference Threads section for any\n";
            a_os << "    thread whose RBX equals 0x"
                 << std::hex << m.rbx << std::dec
                 << " — that's the producer\n";
            a_os << "    that should have signaled this handle.\n";
        } else if (!inSiteA && !inSiteB) {
            a_os << "\n";
            a_os << "    Verdict: main is in some kernel wait we don't yet\n";
            a_os << "    recognise. RIP="
                 << std::format("0x{:016x}", m.rip)
                 << ", RBX="
                 << std::format("0x{:016x}", m.rbx) << ".\n";
            a_os << "    The BSSpinLock-owner search above may still pin\n";
            a_os << "    down a worker-side livelock independent of where\n";
            a_os << "    main is parked.\n";
        }
    }

}
