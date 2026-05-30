#include "PCH.h"
#include "snapshot/Papyrus.h"

namespace FreezeLogger::Snapshot::Papyrus {

    namespace {

        // NOTE on safety: this whole section runs inside Reporter::Section,
        // which wraps it in both an SEH __except and a C++ try/catch. So we
        // do NOT need local __try here (and must not, per MSVC's C2712 rule
        // since we construct std::string/format temporaries). A fault while
        // reading VM internals aborts only this section; everything written
        // before the fault is preserved in the stream buffer.
        //
        // All reads below are plain member loads (and BSTHashMap::size(),
        // which is just `_capacity - _free` — two uint32 reads, no traversal
        // and no lock). We deliberately take NONE of the VM's BSSpinLocks:
        // grabbing an engine lock from the watchdog thread at freeze time is
        // exactly the kind of lock-order inversion this whole project exists
        // to avoid. The running-stack enumeration is therefore a best-effort,
        // lock-free read and may observe a torn map; it is capped and the
        // outer SEH catches any access violation.

        constexpr int kMaxStackDetail = 16;   // per-stack lines we print
        constexpr int kMaxStackVisit  = 512;  // hard cap on iteration

        const char* StateString(RE::BSScript::Stack::State a_s) {
            using S = RE::BSScript::Stack::State;
            switch (a_s) {
            case S::kRunning:                          return "running";
            case S::kFinished:                         return "finished";
            case S::kWaitingOnMemory:                  return "waiting:memory";
            case S::kWaitingOnLatentFunction:          return "waiting:latent-fn";
            case S::kWaitingOnOtherStackForCall:       return "waiting:other-stack-call";
            case S::kWaitingOnOtherStackForReturn:     return "waiting:other-stack-return";
            case S::kWaitingOnOtherStackForReturnNoPop:return "waiting:other-stack-return-nopop";
            case S::kRetryReturnNoPop:                 return "retry:return-nopop";
            case S::kRetryCall:                        return "retry:call";
            default:                                   return "<unknown>";
            }
        }

        const char* FreezeStateString(RE::BSScript::Stack::FreezeState a_f) {
            using F = RE::BSScript::Stack::FreezeState;
            switch (a_f) {
            case F::kUnfrozen: return "unfrozen";
            case F::kFreezing: return "freezing";
            case F::kFrozen:   return "frozen";
            default:           return "<unknown>";
            }
        }

        void WriteCounts(std::ostream& a_os,
                         RE::BSScript::Internal::VirtualMachine* a_vm) {
            a_os << "Initialized:         "
                 << (a_vm->initialized ? "yes" : "no") << "\n";
            a_os << "Overstressed:        "
                 << (a_vm->overstressed ? "yes (VM is shedding load)" : "no")
                 << "\n";
            a_os << "Running stacks:      "
                 << a_vm->allRunningStacks.size() << "\n";
            a_os << "Waiting latent:      "
                 << a_vm->waitingLatentReturns.size()
                 << "   (stacks parked on a latent return)\n";
            a_os << "Attached scripts:    "
                 << a_vm->attachedScripts.size() << " handles\n";
            a_os << "Script arrays live:  "
                 << a_vm->arrays.size() << "\n";
            a_os << "Pending func msgs:   "
                 << a_vm->uiWaitingFunctionMessages
                 << "   (queued cross-thread calls awaiting VM dispatch)\n";
            a_os << "  overflow queue:    "
                 << a_vm->overflowFuncMsgs.size() << "\n";
            a_os << "Suspend overflow:    A="
                 << a_vm->overflowSuspendArray1.size()
                 << "  B=" << a_vm->overflowSuspendArray2.size() << "\n";
            a_os << "Queued unbinds:      "
                 << a_vm->queuedUnbinds.size() << "\n";
        }

        // Best-effort, lock-free walk of the running-stack map. Reads each
        // Stack's state/freeze/frame-count. Histogram covers everything we
        // visit (capped); the per-line detail is limited to the first
        // kMaxStackDetail entries to keep the report bounded.
        void WriteRunningStackDetail(
            std::ostream& a_os,
            RE::BSScript::Internal::VirtualMachine* a_vm) {

            a_os << "\nRunning-stack detail (best-effort, lock-free; "
                    "may be racy):\n";

            int visited      = 0;
            int detailed     = 0;
            int stRunning    = 0;
            int stWaiting    = 0;
            int stFinished   = 0;
            int stOther      = 0;
            int frozenCount  = 0;

            for (const auto& entry : a_vm->allRunningStacks) {
                if (visited >= kMaxStackVisit) break;
                ++visited;

                const auto& stackPtr = entry.second;
                auto* stack = stackPtr.get();
                if (!stack) {
                    continue;
                }

                const auto state = stack->state.get();
                const auto freeze = stack->freezeState.get();

                using S = RE::BSScript::Stack::State;
                switch (state) {
                case S::kRunning:  ++stRunning;  break;
                case S::kFinished: ++stFinished; break;
                case S::kWaitingOnMemory:
                case S::kWaitingOnLatentFunction:
                case S::kWaitingOnOtherStackForCall:
                case S::kWaitingOnOtherStackForReturn:
                case S::kWaitingOnOtherStackForReturnNoPop:
                    ++stWaiting; break;
                default: ++stOther; break;
                }
                if (freeze == RE::BSScript::Stack::FreezeState::kFrozen) {
                    ++frozenCount;
                }

                if (detailed < kMaxStackDetail) {
                    ++detailed;
                    a_os << std::format(
                        "  [stackID {:>6}] state={:<28} freeze={:<9} "
                        "frames={}\n",
                        static_cast<std::uint32_t>(stack->stackID),
                        StateString(state),
                        FreezeStateString(freeze),
                        stack->frames);
                }
            }

            if (visited == 0) {
                a_os << "  <no running stacks>\n";
                return;
            }

            if (detailed < visited) {
                a_os << std::format(
                    "  ... {} more not shown (visited {} of {} reported)\n",
                    visited - detailed, visited,
                    a_vm->allRunningStacks.size());
            }
            a_os << std::format(
                "  state summary: running={} waiting={} finished={} "
                "other={}  |  frozen={}\n",
                stRunning, stWaiting, stFinished, stOther, frozenCount);

            a_os << "\nReading: a healthy idle VM shows a handful of running "
                    "stacks and 0 pending func msgs. A large 'Pending func "
                    "msgs' or 'Waiting latent' backlog, or many 'waiting' "
                    "stacks, points at a script-side stall (runaway mod "
                    "script, stuck latent call) rather than the native "
                    "WaitForJobTask hang tracked in case-study 27.\n";
        }

    }

    void Write(std::ostream& a_os) {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            a_os << "<Papyrus VM singleton unavailable>\n";
            return;
        }
        a_os << "VM singleton:        0x"
             << std::format("{:x}", reinterpret_cast<std::uintptr_t>(vm))
             << "\n";

        WriteCounts(a_os, vm);
        WriteRunningStackDetail(a_os, vm);
    }

}
