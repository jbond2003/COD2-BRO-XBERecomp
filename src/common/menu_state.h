#pragma once

// what the menu mouse (src/game/menu_mouse.cpp) tells the rest
namespace game {

bool MenuMouseCursorVisible();  // a menu has had the pointer in the last few frames: draw it
bool MenuHasPointer();          // ... in the last two: the mouse is not looking around
// each picture shown: while a front end menu is up on a widescreen output, only the band of the frame its screens
// are drawn in is shown, over the whole picture (the game's menus have black above and below them)
void UpdateFrameBand();
// each frame's command (CL_MouseMove): a vehicle that jumped or lies on its side goes in the log (src/game/high_fps.cpp)
void WatchVehicles();

}  // namespace game
