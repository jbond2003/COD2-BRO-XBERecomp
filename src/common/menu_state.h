#pragma once

#include <cstdint>
#include <memory>

#include "gpu/texture_override.h"

// what the menu mouse (src/game/menu_mouse.cpp) tells the rest
namespace game {

bool MenuMouseCursorVisible();  // a menu has had the pointer in the last few frames: draw it
bool MenuHasPointer();          // ... in the last two: the mouse is not looking around
// each picture shown: while a front end menu is up on a widescreen output, only the band of the frame its screens
// are drawn in is shown, over the whole picture (the game's menus have black above and below them)
void UpdateFrameBand();
// each frame's command (CL_MouseMove): a vehicle that jumped or lies on its side goes in the log (src/game/high_fps.cpp)
void WatchVehicles();
// the Controller screen with the Modern configuration picked (src/game/pad_picture.cpp): its picture (null while
// another is picked or none could be read), and a label drawn where the picture's line for it ends, for the scope
std::shared_ptr<const gpu::texture_override::Picture> ModernPadPicture();
struct ModernPadLabel {
    explicit ModernPadLabel(uint32_t widget);
    ~ModernPadLabel();
    ModernPadLabel(const ModernPadLabel&) = delete;
    ModernPadLabel& operator=(const ModernPadLabel&) = delete;
    uint32_t widget = 0;
    float own_x = 0, own_y = 0;
    uint32_t own_flags = 0;
};

}  // namespace game
