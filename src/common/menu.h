#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace input {
struct Pad;
}

// the F10 settings panel, drawn by DrawUi (GameInfo::ui); Windows only
namespace app {

extern float g_zoom;  // the gameplay camera's zoom as a tangent ratio: 1 unzoomed, under 1 aiming down sights
bool MenuOpen();
void ToggleMenu();
void OpenBindings();  // the panel with the key bindings alone
// PC OPTIONS in the game's Options menu: the same settings and the key bindings on a page like the game's own
// option pages (paper_page.h), mouse, keys and controller
void OpenPcOptions();
bool PcOptionsOpen();
void PcOptionsPad(const input::Pad& pad);  // the controller's report each poll, before the game's is blanked
bool Capturing();  // a controls box is waiting for a key: Escape is the box's, not the panel's
void DrawUi(double fps);

}  // namespace app

// PC Options as one of the game's own screens (src/game/menu_mouse.cpp shows them on the Gameplay screen): its lines
// with their words, and what the screen's buttons do to them. Lines are counted without headings.
namespace app::pc {

struct Line {
    std::string label;
    std::string value;
    bool action = false;  // Reset to Defaults, Apply and Exit
};

std::vector<Line> Lines(int selected);  // selected: a binding line shows which key box is picked
void Step(int line, int dir);           // left or right
void Press(int line);                   // A or Enter: step a setting, wait for a key, reset, or apply
void Clear(int line);                   // X or Delete: a binding's key box emptied
void Update();                          // each poll: a key box waiting for its key
bool Capturing();
void Leave(bool applied);               // the screen closes; display changes not applied are dropped

}  // namespace app::pc
