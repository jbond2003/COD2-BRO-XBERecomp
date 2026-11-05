#pragma once

// what the menu mouse (src/game/menu_mouse.cpp) tells the rest
namespace game {

bool MenuMouseCursorVisible();  // a menu has had the pointer in the last few frames: draw it
bool MenuHasPointer();          // ... in the last two: the mouse is not looking around

}  // namespace game
