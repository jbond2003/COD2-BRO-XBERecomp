#pragma once
#include <cstddef>
#include <string>

#include "core/input.h"

// the keyboard and mouse as the controller on port 0, after BLACK-XBERecomp's
namespace input {

void StartMouse(void* hwnd);     // window thread, the game window is up
void OnRawInput(void* handle);   // window thread, WM_INPUT
void ReleaseCursor();            // the window lost the focus: the clip goes, held keys wait for release
void Keyboard(Pad& pad);         // held keys onto port 0's pad
void SeenPad(const Pad& pad);    // a controller's report before the keys join it: its input hands over to it
bool KeyboardActive();           // keyboard or mouse used last, not a controller
uint32_t GameActions();          // the game's own actions the keys hold in play, bit per action id
bool SkipHeld();                 // a left click, Enter, Space, Escape or a controller face button held: skips a movie
bool Playing();                  // the game took mouse counts in the last moment: the player has the view
// raw mouse counts since the last call, scaled by the settings; false when there are none. Asked by the game's look
// update, which runs in play only: being asked is what Playing reports.
bool TakeMouse(float& dx, float& dy);

struct MenuPointer {
    bool active = false;
    float x = 0, y = 0, width = 0, height = 0;
    uint32_t buttons = 0, pressed = 0;  // LMB bit 0, RMB bit 1
    int wheel = 0;
};
MenuPointer ReadMenuPointer();
void SetMenuInput(bool active);  // explicit FEng ownership, independent of the gameplay look timeout
void UseMenuMouse();
void MenuResume();             // a short Start press through the normal pause handling
// a short press of the dpad (Pad::buttons bits) and the analog buttons (bit i is Pad::analog[i]), for a menu that
// reads the controller itself rather than FEng's messages: the controller layout page's preset arrows
void MenuTap(uint16_t buttons, uint8_t analog = 0);
void ApplyMenuTap(Pad& pad);   // port 0's report: the tap joins it
// a name entry keyboard screen (passwords, profile and save names) is up: the keys type into it, Enter finishes,
// Backspace deletes and Escape backs out, instead of acting as bound
void SetTextEntry(bool on);
bool TextEntry();             // ... and a menu has the keys
void OnChar(unsigned ch);      // window thread, WM_CHAR
char TakeTyped();              // the next character typed for the screen, upper case, '\b' delete, '\r' finish; 0 none
void TypeAccept();             // A tapped as the controller would, for the key TakeTyped's caller focused

// actions the keyboard and mouse can be bound to in the settings file's [keys], key names comma separated
// letter, digit and symbol keys bind by position as the US key there
struct Action {
    const char* id;
    const char* defaults;
};
const Action* Actions(size_t* count);
void LoadBindings();  // after the settings load
// the F10 panel's controls: an action's name, its key in slot 0 or 1 (empty for none), binding one (vk 0 clears;
// a key the other slot has moves), the defaults back, and a bindable key or mouse button held now (0 for none)
const char* Label(size_t action);
std::string Binding(size_t action, int slot);
void Bind(size_t action, int vk, int slot);
void ResetBindings();
int PressedKey();
// the first key bound to an action by its settings id ("action", "jump" ...), empty for none; the version moves
// whenever a binding does, for whoever shows them
std::string KeyName(const char* id);
uint32_t BindingsVersion();

}  // namespace input
