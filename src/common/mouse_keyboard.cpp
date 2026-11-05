// the keyboard and mouse as the controller on port 0
#include "common/mouse_keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/menu.h"
#include "common/options.h"

namespace input {

namespace {

HWND g_hwnd = nullptr;
std::atomic<int> g_dx{0};
std::atomic<int> g_dy{0};
std::atomic<uint64_t> g_take_ms{0};  // TakeMouse, asked every tick in play
std::atomic<bool> g_settle{false};
std::atomic<bool> g_keyboard{true};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_text_entry{false};
std::mutex g_typed_lock;
std::string g_typed;  // characters typed for a name entry screen, oldest first
std::atomic<uint32_t> g_menu_pressed{0}, g_block_mouse{0};
std::atomic<int> g_menu_wheel{0};
std::atomic<uint64_t> g_resume_until{0};
std::atomic<uint64_t> g_accept_until{0};  // a name entry keyboard's A, pressed for a typed key
std::atomic<uint32_t> g_game_actions{0};  // the game's actions the keys hold, read by the game's input update
constexpr uint64_t kPlayMs = 250;
constexpr uint64_t kTapMs = 80;
// The wheel binds as two keys, codes Windows leaves unassigned. A notch holds what is bound to it for one tap.
constexpr int kWheelUp = 0x0E;
constexpr int kWheelDown = 0x0F;
std::atomic<uint64_t> g_wheel_ms[2] = {};  // the last notch up, down

uint64_t NowMs() {
    auto since = std::chrono::steady_clock::now().time_since_epoch();
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

// what an action presses: 0-7 analog A B X Y black white LT RT, 8-15 the buttons bits (dpad up down left right,
// start, back, left thumb, right thumb), 16-19 the move keys, 20-23 the look keys
struct Target {
    Action action;
    int button;
    int game = -1;  // the game's own action, pressed directly in play whatever controller layout is picked
    const char* command = nullptr;  // kCommand: run when the key goes down in play
};
constexpr int kWalk = -3;
constexpr int kCommand = -4;
constexpr int kFirstButtonBit = 8;
constexpr int kStart = 12;
constexpr int kFirstMoveKey = 16;
constexpr int kFirstLookKey = 20;
constexpr int kStickMax = 32767;
constexpr int kMoveDpad[4] = {0, 1, 2, 3};  // forward back left right: dpad up down left right bits
// CoD2: Big Red One's default controller layout (config/default.cfg), the button each key presses; another layout
// picked in the game's options moves them in play (src/game/mouse_look.cpp, RemapToLayout)
constexpr Target kActions[] = {
    {{"move_forward", "W"}, 16},
    {{"move_back", "S"}, 17},
    {{"strafe_left", "A"}, 18},
    {{"strafe_right", "D"}, 19},
    {{"walk", ""}, kWalk},
    {{"fire", "LMB"}, 7},                 // right trigger
    {{"aim", "RMB"}, 6},                  // left trigger: aim down the sights
    {{"use", "F"}, 0},                    // A
    {{"crouch", "C"}, kCommand, -1, "togglecrouch"},  // crouch, again to stand
    {{"prone", "Z"}, kCommand, -1, "toggleprone"},    // prone, again to stand
    {{"reload", "R"}, 2},                 // X
    {{"jump", "Space"}, 3},               // Y: raise stance, climb
    {{"melee", "V,MMB"}, 4},              // black
    {{"grenade", "G"}, 5},                // white
    {{"sprint", "Shift"}, 14},            // left stick click
    {{"right_stick", "X"}, 15},           // right stick click
    {{"next_weapon", "1,Wheel Up,Wheel Down"}, 8},  // dpad up
    {{"dpad_down", "2"}, 9},
    {{"lean_left", "Q"}, 10},             // dpad left
    {{"lean_right", "E"}, 11},            // dpad right
    {{"objectives", "Tab"}, 13},          // back
    {{"pause", "Escape"}, kStart},
    {{"look_up", "Up"}, 20},
    {{"look_down", "Down"}, 21},
    {{"look_left", "Left"}, 22},
    {{"look_right", "Right"}, 23},
};
constexpr size_t kCount = std::size(kActions);
struct Name {
    int vk;
    const char* name;
};
constexpr Name kNames[] = {
    {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
    {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
    {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
    {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
    {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
    {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
    {'9', "9"},
    {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
    {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F11, "F11"}, {VK_F12, "F12"},
    {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
    {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
    {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
    {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
    {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
    {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

// not bindable: in the game's menus the arrows are the dpad, Enter accepts, Backspace backs out
constexpr int kFixed[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_BACK};

// US layout scan codes of the keys that move with the layout: a bind names the US key at that position
struct Position {
    int vk;
    UINT scan;
};
constexpr Position kPositions[] = {
    {'1', 0x02}, {'2', 0x03}, {'3', 0x04}, {'4', 0x05}, {'5', 0x06}, {'6', 0x07}, {'7', 0x08}, {'8', 0x09},
    {'9', 0x0A}, {'0', 0x0B}, {VK_OEM_MINUS, 0x0C}, {VK_OEM_PLUS, 0x0D},
    {'Q', 0x10}, {'W', 0x11}, {'E', 0x12}, {'R', 0x13}, {'T', 0x14}, {'Y', 0x15}, {'U', 0x16}, {'I', 0x17},
    {'O', 0x18}, {'P', 0x19}, {VK_OEM_4, 0x1A}, {VK_OEM_6, 0x1B},
    {'A', 0x1E}, {'S', 0x1F}, {'D', 0x20}, {'F', 0x21}, {'G', 0x22}, {'H', 0x23}, {'J', 0x24}, {'K', 0x25},
    {'L', 0x26}, {VK_OEM_1, 0x27}, {VK_OEM_7, 0x28}, {VK_OEM_3, 0x29}, {VK_OEM_5, 0x2B},
    {'Z', 0x2C}, {'X', 0x2D}, {'C', 0x2E}, {'V', 0x2F}, {'B', 0x30}, {'N', 0x31}, {'M', 0x32},
    {VK_OEM_COMMA, 0x33}, {VK_OEM_PERIOD, 0x34}, {VK_OEM_2, 0x35},
};

using Binds = std::array<std::vector<int>, kCount>;
std::mutex g_bind_lock;
Binds g_binds;

int Vk(const std::string& name) {
    for (const Name& n : kNames)
        if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
    return 0;
}

UINT ScanOf(int vk) {
    for (const Position& p : kPositions)
        if (p.vk == vk) return p.scan;
    return 0;
}

// the keyboard layout the player types with, switched per window thread
HKL Layout() { return GetKeyboardLayout(GetWindowThreadProcessId(g_hwnd, nullptr)); }

// the key the current layout has at the bind's position
int LiveVk(int vk) {
    UINT scan = ScanOf(vk);
    if (!scan) return vk;
    UINT live = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, Layout());
    return live ? int(live) : vk;
}

std::vector<int> Parse(const std::string& list) {
    std::vector<int> out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string name = list.substr(start, comma - start);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        if (int vk = Vk(name)) out.push_back(vk);
        start = comma + 1;
    }
    return out;
}

// the F10 panel has the keys and mouse while it is open
bool Active() { return g_hwnd && GetForegroundWindow() == g_hwnd && !app::MenuOpen(); }
bool Wheel(int vk) { return vk == kWheelUp || vk == kWheelDown; }
bool Down(int vk) {
    if (Wheel(vk)) return NowMs() - g_wheel_ms[vk == kWheelDown] < kTapMs;
    return GetAsyncKeyState(LiveVk(vk)) < 0;
}

void Clip() {
    RECT r;
    RECT cur;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    if (!GetClipCursor(&cur) || !EqualRect(&r, &cur)) ClipCursor(&r);
}

// Windows asks for the cursor only when the mouse moves
void RefreshCursor() {
    if (g_hwnd) PostMessageW(g_hwnd, WM_SETCURSOR, WPARAM(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
}

void SetKeyboard(bool on) {
    if (g_keyboard.exchange(on) != on) RefreshCursor();
}

// keys held from before the game had the input wait for release; true once none is held
bool UpdateSettle(const Binds& binds) {
    if (!g_settle) return true;
    for (auto& b : binds)
        for (int vk : b)
            if (Down(vk)) return false;
    for (int vk : kFixed)
        if (Down(vk)) return false;
    g_settle = false;
    return true;
}

// the bound keys held this tick, beside the pad buttons they pressed
struct Held {
    bool used = false;
    bool walk = false;
    uint32_t game = 0;      // the game's actions held, bit per action id (all under 32)
    uint32_t commands = 0;  // kCommand actions held, bit per action index
    int stick[8] = {};  // move forward back left right, look up down left right
};

Held PressActions(const Binds& binds, bool play, Pad& p) {
    Held held;
    // in a menu the wheel and the mouse buttons do nothing, a bound key presses its button
    auto pressed = [&](size_t action, int vk) {
        constexpr int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        uint32_t mask = 0;
        for (int i = 0; i < 5; i++) if (buttons[i] == vk) mask = 1u << i;
        bool mouse = Wheel(vk) || mask;
        return Down(vk) && (play || !mouse) && !(mask & g_block_mouse.load());
    };
    for (size_t i = 0; i < kCount; i++) {
        int b = kActions[i].button;
        if (!std::any_of(binds[i].begin(), binds[i].end(), [&](int vk) { return pressed(i, vk); })) continue;
        held.used = true;
        if (b == kWalk) {
            held.walk = true;
        } else if (b == kCommand) {
            if (play) held.commands |= 1u << i;
        } else if (play && kActions[i].game >= 0) {
            held.game |= 1u << kActions[i].game;
        } else if (b >= kFirstLookKey && !play) {
            continue;
        } else if (b >= kFirstMoveKey) {
            held.stick[b - kFirstMoveKey] = 1;
            // menus that only follow the dpad move with WASD too
            if (!play) p.buttons |= uint16_t(1u << kMoveDpad[b - kFirstMoveKey]);
        } else if (b < kFirstButtonBit) {
            p.analog[b] = 0xFF;
        } else {
            // Start pauses and resumes; keep the pause binding identical in play and menus.
            p.buttons |= uint16_t(1u << (b - kFirstButtonBit));
        }
    }
    return held;
}

}  // namespace

void StartMouse(void* hwnd) {
    g_hwnd = static_cast<HWND>(hwnd);
    RAWINPUTDEVICE mouse{1, 2, 0, g_hwnd};  // generic desktop page, mouse usage
    RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
}

void OnRawInput(void* handle) {
    RAWINPUT ri;
    UINT size = sizeof(ri);
    HRAWINPUT input = static_cast<HRAWINPUT>(handle);
    if (GetRawInputData(input, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
    if (ri.header.dwType != RIM_TYPEMOUSE || !Active()) return;
    const RAWMOUSE& m = ri.data.mouse;
    if (m.usButtonFlags & RI_MOUSE_WHEEL) g_wheel_ms[SHORT(m.usButtonData) < 0] = NowMs();
    if (g_menu) {
        if (m.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) g_menu_pressed.fetch_or(1);
        if (m.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) g_menu_pressed.fetch_or(2);
        if (m.usButtonFlags & RI_MOUSE_WHEEL) g_menu_wheel += SHORT(m.usButtonData);
    }
    // the cursor is free outside play
    if (!Playing()) {
        ClipCursor(nullptr);
        return;
    }
    SetKeyboard(true);
    Clip();
    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        g_dx += m.lLastX;
        g_dy += m.lLastY;
    }
}

void ReleaseCursor() {
    ClipCursor(nullptr);
    g_settle = true;
    g_menu_pressed = 0;
    g_menu_wheel = 0;
    g_resume_until = 0;
}

bool Playing() { return !g_menu && NowMs() - g_take_ms < kPlayMs; }

bool TakeMouse(float& dx, float& dy) {
    if (g_menu.exchange(false)) {
        g_dx = g_dy = 0;
        g_wheel_ms[0] = g_wheel_ms[1] = 0;
        RefreshCursor();
    }
    uint64_t now = NowMs();
    bool stale = now - g_take_ms.exchange(now) >= kPlayMs;
    if (stale) RefreshCursor();
    int x = g_dx.exchange(0);
    int y = g_dy.exchange(0);
    if ((!x && !y) || stale || !Active()) return false;
    const game::Options& o = game::Opt();
    dx = float(o.invert_mouse_x ? -x : x) * o.mouse_sensitivity;
    dy = float(o.invert_mouse ? -y : y) * o.mouse_vertical_sensitivity;
    return true;
}

void Keyboard(Pad& p) {
    constexpr int mouse_buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    uint32_t mouse_down = 0;
    for (int i = 0; i < 5; i++) if (Down(mouse_buttons[i])) mouse_down |= 1u << i;
    uint32_t blocked = g_block_mouse.load() & mouse_down;
    if (g_menu || g_settle || !Active()) blocked |= mouse_down;
    g_block_mouse = blocked;
    if (!Active()) {
        g_settle = true;
        g_game_actions = 0;
        return;
    }
    Binds binds;
    bool typing = g_text_entry && g_menu;
    if (!typing) {
        std::lock_guard lock(g_bind_lock);
        binds = g_binds;
    }
    if (!UpdateSettle(binds)) {
        g_game_actions = 0;
        return;
    }
    bool play = Playing();
    bool fixed_down = std::any_of(std::begin(kFixed), std::end(kFixed), Down);
    Held held = PressActions(binds, play, p);
    g_game_actions = held.game;
    static uint32_t commands_held = 0;
    for (size_t i = 0; i < kCount; i++)
        if ((held.commands >> i & 1) && !(commands_held >> i & 1)) QueueCommand(kActions[i].command);
    commands_held = held.commands;
    // Enter and Backspace act in menus as A and B (the button prompts say so); in play they do what they are bound to.
    // On a name entry screen they type (TakeTyped), and Escape backs out
    if (typing) {
        if (Down(VK_ESCAPE)) p.analog[1] = 0xFF;
        if (NowMs() < g_accept_until) p.analog[0] = 0xFF;
    } else {
        if (!play && Down(VK_RETURN)) p.analog[0] = 0xFF;
        if (!play && Down(VK_BACK)) p.analog[1] = 0xFF;
    }
    if (NowMs() < g_resume_until) p.buttons |= 0x10;
    ApplyMenuTap(p);
    // the arrows, kFixed's first four, are the dpad's bits
    if (!play) {
        for (int i = 0; i < 4; i++) p.buttons |= uint16_t(Down(kFixed[i]) << i);
    }
    const int* stick = held.stick;
    float speed = held.walk ? float(std::clamp(game::Opt().walk_speed, 10u, 100u)) / 100.0f : 1.0f;
    if (int x = stick[3] - stick[2]) p.lx = int16_t(float(x) * speed * float(kStickMax));
    if (int y = stick[0] - stick[1]) p.ly = int16_t(float(y) * speed * float(kStickMax));
    if (int x = stick[7] - stick[6]) p.rx = int16_t(x * kStickMax);
    if (int y = stick[4] - stick[5]) p.ry = int16_t(y * kStickMax);
    if (fixed_down || held.used) SetKeyboard(true);
}

void SeenPad(const Pad& p) {
    constexpr int16_t kStickPushed = 16000;
    constexpr uint8_t kAnalogPushed = 0x20;
    auto pushed = [](int16_t v) { return v > kStickPushed || v < -kStickPushed; };
    auto pressed = [](uint8_t a) { return a > kAnalogPushed; };
    bool analog = std::any_of(std::begin(p.analog), std::end(p.analog), pressed);
    bool sticks = pushed(p.lx) || pushed(p.ly) || pushed(p.rx) || pushed(p.ry);
    if (p.buttons || analog || sticks) SetKeyboard(false);
}

bool KeyboardActive() { return g_keyboard; }
uint32_t GameActions() { return g_game_actions; }
bool SkipHeld() {
    if (HeldOnAnyController()) return true;
    return Active() && (Down(VK_LBUTTON) || Down(VK_RETURN) || Down(VK_SPACE) || Down(VK_ESCAPE));
}

MenuPointer ReadMenuPointer() {
    MenuPointer p;
    p.pressed = g_menu_pressed.exchange(0);
    p.wheel = g_menu_wheel.exchange(0);
    POINT point;
    RECT rect;
    if (!Active() || !GetCursorPos(&point) || !ScreenToClient(g_hwnd, &point) || !GetClientRect(g_hwnd, &rect)) return p;
    p.active = true;
    p.x = float(point.x); p.y = float(point.y);
    p.width = float(rect.right); p.height = float(rect.bottom);
    p.buttons = uint32_t(Down(VK_LBUTTON)) | uint32_t(Down(VK_RBUTTON)) << 1;
    return p;
}

void SetMenuInput(bool active) {
    if (g_menu.exchange(active) != active) {
        g_dx = g_dy = 0;
        g_wheel_ms[0] = g_wheel_ms[1] = 0;
        g_menu_pressed = 0;
        g_menu_wheel = 0;
        RefreshCursor();
    }
    if (active) {
        // A mouse click which closes a menu must be released before it can become fire/aim in play.
        constexpr int buttons[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        uint32_t down = 0;
        for (int i = 0; i < 5; i++) if (Down(buttons[i])) down |= 1u << i;
        g_block_mouse.fetch_or(down);
    }
}

void UseMenuMouse() { SetKeyboard(true); }

bool TextEntry() { return g_text_entry && g_menu; }

void SetTextEntry(bool on) {
    if (g_text_entry.exchange(on) == on) return;
    std::lock_guard lock(g_typed_lock);
    g_typed.clear();
}

void OnChar(unsigned ch) {
    if (!g_text_entry || !g_menu || !Active()) return;
    char c = 0;
    if (ch >= 'a' && ch <= 'z') c = char(ch - 'a' + 'A');
    else if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '.' || ch == '@' ||
             ch == '\b' || ch == '\r') c = char(ch);
    if (!c) return;
    SetKeyboard(true);
    std::lock_guard lock(g_typed_lock);
    if (g_typed.size() < 32) g_typed += c;
}

char TakeTyped() {
    // one at a time: the last one's A tap and a gap as long, so the game sees each press
    if (NowMs() < g_accept_until + kTapMs) return 0;
    std::lock_guard lock(g_typed_lock);
    if (g_typed.empty()) return 0;
    char c = g_typed.front();
    g_typed.erase(0, 1);
    return c;
}
void MenuResume() { g_resume_until = NowMs() + kTapMs; }
void TypeAccept() { g_accept_until = NowMs() + kTapMs; }

const Action* Actions(size_t* count) {
    static const auto list = [] {
        std::array<Action, kCount> a{};
        for (size_t i = 0; i < kCount; i++) a[i] = kActions[i].action;
        return a;
    }();
    *count = kCount;
    return list.data();
}

namespace {

// the F10 panel's names for the actions, in kActions' order
constexpr const char* kLabels[] = {
    "MOVE FORWARD", "MOVE BACK", "STRAFE LEFT", "STRAFE RIGHT", "WALK", "FIRE", "AIM DOWN SIGHTS", "USE",
    "CROUCH", "PRONE", "RELOAD", "JUMP / STAND", "MELEE", "THROW GRENADE", "SPRINT", "RIGHT STICK CLICK",
    "NEXT WEAPON", "D-PAD DOWN", "LEAN LEFT", "LEAN RIGHT", "OBJECTIVES", "PAUSE", "LOOK UP", "LOOK DOWN",
    "LOOK LEFT", "LOOK RIGHT",
};
static_assert(std::size(kLabels) == kCount);

const char* NameOf(int vk) {
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return nullptr;
}

std::string Join(const std::vector<int>& vks) {
    std::string s;
    for (int vk : vks) {
        if (!s.empty()) s += ',';
        s += NameOf(vk);
    }
    return s;
}

}  // namespace

const char* Label(size_t action) { return action < kCount ? kLabels[action] : ""; }

std::atomic<uint32_t> g_bind_version{0};
uint32_t BindingsVersion() { return g_bind_version.load(); }

std::string KeyName(const char* id) {
    for (size_t i = 0; i < kCount; i++) {
        if (std::strcmp(kActions[i].action.id, id) != 0) continue;
        std::string key = Binding(i, 0);
        return key.empty() ? Binding(i, 1) : key;
    }
    return {};
}

std::string Binding(size_t action, int slot) {
    std::lock_guard lock(g_bind_lock);
    if (action >= kCount || slot < 0 || size_t(slot) >= g_binds[action].size()) return {};
    const char* name = NameOf(g_binds[action][size_t(slot)]);
    return name ? name : "";
}

void Bind(size_t action, int vk, int slot) {
    if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) return;
    {
        std::lock_guard lock(g_bind_lock);
        std::vector<int>& keys = g_binds[action];
        int slots[2] = {0, 0};
        for (size_t i = 0; i < keys.size() && i < 2; i++) slots[i] = keys[i];
        if (vk && slots[1 - slot] == vk) slots[1 - slot] = 0;
        slots[slot] = vk;
        keys.clear();
        for (int k : slots)
            if (k) keys.push_back(k);
        game::Opt().keys[kActions[action].action.id] = Join(keys);
    }
    g_bind_version++;
    app::Save();
}

void ResetBindings() {
    game::Opt().keys.clear();
    LoadBindings();
    app::Save();
}

int PressedKey() {
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) return 0;
    for (const Name& n : kNames)
        if (Down(n.vk)) return n.vk;
    return 0;
}

void LoadBindings() {
    std::lock_guard lock(g_bind_lock);
    const auto& keys = game::Opt().keys;
    for (size_t i = 0; i < kCount; i++) {
        auto it = keys.find(kActions[i].action.id);
        g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
    }
    g_bind_version++;
}

}  // namespace input

#else
// the headless harness: no keyboard or mouse
#include "app/app.h"
#include "common/options.h"
namespace input {
void StartMouse(void*) {}
void OnRawInput(void*) {}
void ReleaseCursor() {}
void Keyboard(Pad&) {}
void SeenPad(const Pad&) {}
// harness test: COD2_KBM plays as a keyboard player, for the key prompts
bool KeyboardActive() { return std::getenv("COD2_KBM") != nullptr; }
// the harness tests' clock: seconds since the process started, as the log stamps
const auto g_start = std::chrono::steady_clock::now();
double Seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }
// harness test: COD2_KEY_ACTIONS="22@110-114,10@116-116.2" holds action id over seconds since start
uint32_t GameActions() {
    struct Hold { uint32_t id; double from, to; };
    static const std::vector<Hold> holds = [] {
        std::vector<Hold> out;
        for (const char* p = std::getenv("COD2_KEY_ACTIONS"); p && *p;) {
            Hold h;
            int used = 0;
            if (std::sscanf(p, " %u@%lf-%lf%n", &h.id, &h.from, &h.to, &used) != 3) break;
            out.push_back(h);
            p += used;
            if (*p == ',') p++;
        }
        return out;
    }();
    double now = Seconds();
    uint32_t mask = 0;
    for (const Hold& h : holds) if (now >= h.from && now < h.to) mask |= 1u << h.id;
    return mask;
}
// harness test: COD2_SKIP="2-2.2" holds a skip press over seconds since start
bool SkipHeld() {
    double from = 0, to = 0;
    const char* e = std::getenv("COD2_SKIP");
    if (!e || std::sscanf(e, "%lf-%lf", &from, &to) != 2) return false;
    double now = Seconds();
    return now >= from && now < to;
}
bool Playing() { return false; }
bool TakeMouse(float&, float&) { return false; }
// harness test: COD2_CLICK="494,327@23,100,100@30" points at x,y of a 640x480 window and left clicks there at that
// second since start, one after the other
MenuPointer ReadMenuPointer() {
    struct Click { float x, y; double at; };
    static const std::vector<Click> clicks = [] {
        std::vector<Click> out;
        for (const char* p = std::getenv("COD2_CLICK"); p && *p;) {
            Click c;
            int used = 0;
            if (std::sscanf(p, " %f,%f@%lf%n", &c.x, &c.y, &c.at, &used) != 3) break;
            out.push_back(c);
            p += used;
            if (*p == ',') p++;
        }
        return out;
    }();
    static size_t next = 0;
    MenuPointer m;
    if (clicks.empty()) return m;
    double now = Seconds();
    const Click& c = clicks[std::min(next, clicks.size() - 1)];
    m.active = true;
    // a client the guest frame fills, 4:3 or stretched over 16:9 (game::menu::MapPointer)
    float k = app::Widescreen() ? (16.0f / 9.0f) / (4.0f / 3.0f) : 1.0f;
    m.x = c.x * k, m.y = c.y, m.width = 640 * k, m.height = 480;
    if (next < clicks.size() && now >= c.at) {
        m.pressed = 1;
        std::fprintf(stderr, "harness: click at %.0f,%.0f\n", c.x, c.y);
        next++;
    }
    return m;
}
void SetMenuInput(bool) {}
void UseMenuMouse() {}
// harness test: COD2_TYPE="Ab 1\b" types into a name entry screen, a character every quarter second once it is up
std::atomic<bool> g_text_entry{false};
double g_entry_since = 0;
size_t g_typed_at = 0;
bool TextEntry() { return g_text_entry; }
void SetTextEntry(bool on) {
    if (on && !g_text_entry) g_entry_since = Seconds();
    g_text_entry = on;
}
void OnChar(unsigned) {}
char TakeTyped() {
    const char* text = std::getenv("COD2_TYPE");
    if (!text || !g_text_entry) return 0;
    double due = g_entry_since + 1.0 + 0.25 * double(g_typed_at);
    if (Seconds() < due || g_typed_at >= std::strlen(text)) return 0;
    return text[g_typed_at++];
}
void TypeAccept() {}
void MenuResume() {}
const Action* Actions(size_t* count) {
    *count = 0;
    return nullptr;
}
void LoadBindings() {}
const char* Label(size_t) { return ""; }
std::string Binding(size_t, int) { return {}; }
void Bind(size_t, int, int) {}
void ResetBindings() {}
int PressedKey() { return 0; }
// harness test: COD2_KBM names the default keys, as a keyboard player sees them
std::string KeyName(const char* id) {
    constexpr std::pair<const char*, const char*> kDefaults[] = {
        {"use", "F"},        {"fire", "LMB"},       {"jump", "Space"}, {"crouch", "C"}, {"pause", "Escape"},
        {"objectives", "Tab"}, {"prone", "Z"}, {"grenade", "G"},   {"aim", "RMB"},    {"reload", "R"}, {"melee", "V"},
        {"sprint", "Shift"},  {"next_weapon", "1"}, {"lean_left", "Q"}, {"lean_right", "E"}};
    if (!std::getenv("COD2_KBM")) return {};
    for (const auto& [name, key] : kDefaults)
        if (std::strcmp(name, id) == 0) return key;
    return {};
}
uint32_t BindingsVersion() { return 0; }
}  // namespace input
#endif

namespace input {
namespace {
// a menu press the mouse makes as the controller would: dpad bits and the analog buttons pressed, until when
std::atomic<uint32_t> g_tap_buttons{0}, g_tap_analog{0};
std::atomic<int64_t> g_tap_until{0};
int64_t TapClock() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

void MenuTap(uint16_t buttons, uint8_t analog) {
    constexpr int64_t kMenuTapMs = 80;
    g_tap_buttons = buttons;
    g_tap_analog = analog;
    g_tap_until = TapClock() + kMenuTapMs;
}

void ApplyMenuTap(Pad& p) {
    if (TapClock() >= g_tap_until) return;
    p.buttons |= uint16_t(g_tap_buttons);
    for (int i = 0; i < 8; i++)
        if (g_tap_analog & (1u << i)) p.analog[i] = 0xFF;
}
}  // namespace input

namespace input {
namespace {
std::mutex g_command_lock;
std::vector<std::string> g_commands;  // under g_command_lock, oldest first
}  // namespace

void QueueCommand(const std::string& command) {
    std::lock_guard lock(g_command_lock);
    if (g_commands.size() < 64) g_commands.push_back(command);
}

bool TakeCommand(std::string& command) {
    std::lock_guard lock(g_command_lock);
    if (g_commands.empty()) return false;
    command = g_commands.front();
    g_commands.erase(g_commands.begin());
    return true;
}
}  // namespace input
