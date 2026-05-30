#include "PCH.h"
#include "snapshot/AnimGraph.h"

namespace FreezeLogger::Snapshot::AnimGraph {

    namespace {

        // As with the Papyrus section, this runs inside Reporter::Section's
        // SEH + C++ try/catch, so no local __try is needed (or allowed
        // alongside the std::string/format temporaries here). Every access
        // is a member load or a single non-locking accessor; a fault aborts
        // just this section.

        const char* YesNo(bool b) { return b ? "yes" : "no"; }

        void WriteBehaviorGraph(std::ostream& a_os,
                                RE::hkbBehaviorGraph* a_bg) {
            if (!a_bg) {
                a_os << "  behavior graph:    <none>\n";
                return;
            }
            a_os << "  behavior graph:    0x"
                 << std::format("{:x}", reinterpret_cast<std::uintptr_t>(a_bg))
                 << "\n";
            a_os << "    active:          " << YesNo(a_bg->isActive) << "\n";
            a_os << "    linked:          " << YesNo(a_bg->isLinked) << "\n";
            a_os << "    update-active:   " << YesNo(a_bg->updateActiveNodes)
                 << "\n";
            a_os << "    state changed:   "
                 << YesNo(a_bg->stateOrTransitionChanged) << "\n";
            a_os << "    static nodes:    " << a_bg->numStaticNodes << "\n";
            a_os << "    root generator:  "
                 << (a_bg->rootGenerator ? "present" : "<null>") << "\n";
        }

        void WriteActiveGraph(std::ostream& a_os,
                              RE::BShkbAnimationGraph* a_graph) {
            if (!a_graph) {
                a_os << "  active graph:      <null>\n";
                return;
            }
            a_os << "  active graph:      0x"
                 << std::format("{:x}",
                                reinterpret_cast<std::uintptr_t>(a_graph))
                 << "\n";

            const char* proj = a_graph->projectName.c_str();
            a_os << "  project:           "
                 << ((proj && *proj) ? proj : "<empty>") << "\n";
            a_os << "  anim bones:        " << a_graph->numAnimBones << "\n";
            a_os << "  foot IK:           " << YesNo(a_graph->doFootIK != 0)
                 << "\n";

            WriteBehaviorGraph(a_os, a_graph->behaviorGraph);
        }

    }

    void Write(std::ostream& a_os) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            a_os << "<player singleton unavailable>\n";
            return;
        }
        a_os << "Player singleton:  0x"
             << std::format("{:x}", reinterpret_cast<std::uintptr_t>(player))
             << "\n";

        RE::BSTSmartPointer<RE::BSAnimationGraphManager> mgr;
        if (!player->GetAnimationGraphManager(mgr) || !mgr) {
            a_os << "<no animation graph manager on player>\n";
            return;
        }

        const auto graphCount = mgr->graphs.size();
        const auto activeIdx  = mgr->GetRuntimeData().activeGraph;

        a_os << "Graph manager:     0x"
             << std::format("{:x}", reinterpret_cast<std::uintptr_t>(mgr.get()))
             << "\n";
        a_os << "Graphs:            " << graphCount << "\n";
        a_os << "Active graph idx:  " << activeIdx << "\n";

        if (graphCount == 0) {
            a_os << "<player has no animation graphs>\n";
            return;
        }
        if (activeIdx >= graphCount) {
            a_os << "<active graph index out of range; manager may be "
                    "mid-swap>\n";
            return;
        }

        WriteActiveGraph(a_os, mgr->graphs[activeIdx].get());

        a_os << "\nReading: 'active=yes, linked=yes' is the normal running "
                "state. A behavior graph stuck 'active=no' or a project that "
                "is empty/unexpected on the player can indicate an animation "
                "deadlock or a broken behavior swap (e.g. a paralysis / "
                "furniture-idle bug) — distinct from the native "
                "WaitForJobTask hang in case-study 27.\n";
    }

}
