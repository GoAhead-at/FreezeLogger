#include "PCH.h"
#include "snapshot/Engine.h"

namespace FreezeLogger::Snapshot::Engine {

    namespace {

        // Each accessor below is wrapped in its own try/SEH-able helper so a
        // single missing/changed RE field on a non-mainline NG version
        // doesn't take down the whole Engine section.
        //
        // If a particular call fails to compile against your CommonLibSSE-NG
        // version, comment out *just that line* — the rest of the section
        // will still emit. Each line is also tagged with a TODO_RE marker
        // for accessors I haven't independently verified at build time.

        const char* YesNo(bool b) { return b ? "yes" : "no"; }

        void WriteMain(std::ostream& a_os) {
            auto* main = RE::Main::GetSingleton();
            if (!main) {
                a_os << "Main singleton:    <unavailable>\n";
                return;
            }
            // TODO_RE: verify these field names in your NG version.
            a_os << "Quit pending:      " << YesNo(main->quitGame)   << "\n";
            a_os << "Freeze time:       " << YesNo(main->freezeTime) << "\n";
        }

        void WritePlayer(std::ostream& a_os) {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) {
                a_os << "Player singleton:  <unavailable>\n";
                return;
            }

            a_os << "Position:          "
                 << player->GetPositionX() << ", "
                 << player->GetPositionY() << ", "
                 << player->GetPositionZ() << "\n";

            // TODO_RE: GetAngle{X,Y,Z} are typical TESObjectREFR accessors.
            a_os << "Heading (Z rad):   " << player->GetAngleZ() << "\n";

            if (auto* cell = player->GetParentCell()) {
                a_os << "Parent cell:       "
                     << cell->GetFormEditorID() << " ("
                     << (cell->IsInteriorCell() ? "interior" : "exterior")
                     << ")\n";
            } else {
                a_os << "Parent cell:       <none>\n";
            }

            // TODO_RE: GetWorldspace exists on TESObjectREFR / Actor in NG;
            // verify accessor name on 1.5.97 if compile fails.
            if (auto* ws = player->GetWorldspace()) {
                a_os << "Worldspace:        " << ws->GetFormEditorID() << "\n";
            } else {
                a_os << "Worldspace:        <none / interior>\n";
            }
        }

        void WriteUI(std::ostream& a_os) {
            auto* ui = RE::UI::GetSingleton();
            if (!ui) {
                a_os << "UI singleton:      <unavailable>\n";
                return;
            }
            a_os << "Menus paused game: " << YesNo(ui->numPausesGame > 0) << "\n";

            // TODO_RE: MENU_NAME constants may differ in older NG releases.
            // If any of these don't compile, swap the literal in.
            const struct {
                const char*      label;
                std::string_view name;
            } known[] = {
                { "loading screen",     RE::LoadingMenu::MENU_NAME    },
                { "console",            RE::Console::MENU_NAME        },
                { "main menu",          RE::MainMenu::MENU_NAME       },
                { "map",                RE::MapMenu::MENU_NAME        },
                { "inventory",          RE::InventoryMenu::MENU_NAME  },
                { "journal",            RE::JournalMenu::MENU_NAME    },
            };
            std::string openList;
            for (const auto& m : known) {
                if (ui->IsMenuOpen(m.name)) {
                    if (!openList.empty()) openList += ", ";
                    openList += m.label;
                }
            }
            a_os << "Open menus:        "
                 << (openList.empty() ? "<none>" : openList) << "\n";
        }

        void WriteCalendar(std::ostream& a_os) {
            // TODO_RE: RE::Calendar accessors vary across NG versions.
            // GetCurrentGameTime / GetHour / GetDay / GetMonth / GetYear
            // are commonly available; if any fail to compile, comment them.
            auto* cal = RE::Calendar::GetSingleton();
            if (!cal) {
                a_os << "Calendar:          <unavailable>\n";
                return;
            }
            a_os << "Game time (days):  " << cal->GetCurrentGameTime() << "\n";
            a_os << "Hour of day:       " << cal->GetHour()           << "\n";
            a_os << "Day:               " << cal->GetDay()            << "\n";
            a_os << "Month:             " << cal->GetMonth()          << "\n";
            a_os << "Year:              " << cal->GetYear()           << "\n";
        }

    }

    void Write(std::ostream& a_os) {
        WriteMain(a_os);
        WritePlayer(a_os);
        WriteUI(a_os);
        WriteCalendar(a_os);
    }

}
