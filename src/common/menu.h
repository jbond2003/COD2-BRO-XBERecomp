#pragma once
#include <cstdint>

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
