#include "PCH.h"
#include "snapshot/AnimGraph.h"

namespace FreezeLogger::Snapshot::AnimGraph {

    void Write(std::ostream& a_os) {
        // TODO_RE: lite version (player only) for v1.
        //
        //   auto* player = RE::PlayerCharacter::GetSingleton();
        //   if (!player) { a_os << "<no player>\n"; return; }
        //
        //   - current behavior graph file (hkbBehaviorGraph)
        //   - current animation event
        //   - last animation event
        //   - time in current state
        //
        // Confirm accessor name on 1.5.97 (RE::Actor::GetAnimationGraphs?).

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            a_os << "<player singleton unavailable>\n";
            return;
        }
        a_os << "Player singleton:  0x" << std::format("{:x}", (std::uintptr_t)player) << "\n";
        a_os << "<graph state not yet wired up; see TODO_RE in src/snapshot/AnimGraph.cpp>\n";
    }

}
