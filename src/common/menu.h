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

// PC Options as one of the game's own screens (src/game/menu_mouse.cpp shows them on the Gameplay screen): a first
// page of links to a page each for the display, the graphics and the controls. A page's settings change when its
// Apply is picked, not before; key bindings bind at once. The page's lines with their words, and what the screen's
// buttons do to them.
namespace app::pc {
// the first page's last line: where the rest of the settings are
inline constexpr const char* kPanelNote = "Press F10 for every setting";

struct Line {
    std::string label;
    std::string value;
    bool action = false;    // the page's two last lines: Reset to Defaults, then Apply (Apply and Exit on the first)
    bool link = false;      // opens another page: no value, no arrows
    bool note = false;      // words only: the selection passes it
    bool binding = false;   // a key binding: its two keys, the one the selection is on, and whether it waits for a key
    std::string keys[2];
    int slot = 0;
    bool waiting = false;
};

std::vector<Line> Lines(int selected);  // selected: a binding line shows which key box is picked
void Step(int line, int dir);           // left or right
void Press(int line);                   // A or Enter: step a setting, wait for a key, open a page, reset, apply
bool Subpage();                         // on a page, not the first one: its settings wait for its Apply
std::string Title();                    // the page's
uint32_t PageSerial();                  // moves when another page opens: the selection then goes to line Focus()
int Focus();
// B or Escape: from a page to the first one, by way of a page that asks when changes are not applied; false on the
// first page
bool Back();
void Clear(int line);                   // X or Delete: a binding's key box emptied
void Update();                          // each poll: a key box waiting for its key
bool Capturing();
// the game's Gameplay screen's second page (aim assist, hints, the HUD): opened instead of the first page; ExtraDone
// once it is left (Back, Apply or the page that asks), when the screen shows its own lines again
void OpenExtra();
bool ExtraDone();
void Leave(bool applied);               // the screen closes; display changes not applied are dropped

}  // namespace app::pc
