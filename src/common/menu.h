#pragma once
#include <cstdint>

// the F10 settings panel, drawn by DrawUi (GameInfo::ui); Windows only
namespace app {

extern float g_zoom;  // the gameplay camera's zoom as a tangent ratio: 1 unzoomed, under 1 aiming down sights
bool MenuOpen();
void ToggleMenu();
void OpenBindings();  // the panel with the key bindings alone, from PC Options
bool Capturing();  // a controls box is waiting for a key: Escape is the box's, not the panel's
void DrawUi(double fps);

}  // namespace app
