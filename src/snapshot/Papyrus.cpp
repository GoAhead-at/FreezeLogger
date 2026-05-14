#include "PCH.h"
#include "snapshot/Papyrus.h"

namespace FreezeLogger::Snapshot::Papyrus {

    void Write(std::ostream& a_os) {
        // TODO_RE: pull from RE::BSScript::Internal::VirtualMachine::GetSingleton().
        //
        // Targets (verify accessor names against CommonLibSSE-NG headers shipped
        // by the vcpkg port; signatures vary by NG version):
        //
        //   - active stack count       (vm->m_attachedScriptsLock-guarded)
        //   - suspended stack count
        //   - frozen stack count
        //   - top N longest-running stacks (script + function + elapsed time)
        //   - pending event queue depth
        //
        // Until then this is a placeholder so the report renders.

        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            a_os << "<Papyrus VM singleton unavailable>\n";
            return;
        }
        a_os << "VM singleton:      0x" << std::format("{:x}", (std::uintptr_t)vm) << "\n";
        a_os << "<detailed stats not yet wired up; see TODO_RE in src/snapshot/Papyrus.cpp>\n";
    }

}
