#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifdef _WIN32
#include <imgui.h>
#endif
#include <algorithm>
#include <iterator>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/cheats.h"
#include "common/menu.h"
#include "common/menu_state.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/paper_page.h"
#include "common/software_cursor.h"
#include "core/input.h"

namespace game {

void RemapToLayout(const input::Pad& from, input::Pad& to);  // src/game/mouse_look.cpp
void InitPrompts();  // src/game/prompts.cpp
void OptionsPad(input::Pad& pad);  // src/game/menu_mouse.cpp: PC OPTIONS in the Options menu
void ModernPad(input::Pad& pad);  // src/game/pad_layout.cpp: B to crouch, held to go prone

namespace {

// window thread, each message after ImGui's
bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
#ifdef _WIN32
    constexpr intptr_t kKeyWasDown = 1 << 30;
    // F10 arrives as a system key: it would open the window menu
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & kKeyWasDown)) {
        if (wp == VK_F10 || (wp == VK_ESCAPE && app::MenuOpen() && !app::Capturing())) {
            app::ToggleMenu();
            *result = 0;
            return true;
        }
    }
    if (msg == WM_SYSKEYUP && wp == VK_F10) {
        *result = 0;
        return true;
    }
    // a bound Alt must not open the window menu
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU) {
        *result = 0;
        return true;
    }
    if (msg == WM_INPUT) input::OnRawInput(reinterpret_cast<void*>(lp));
    if (msg == WM_CHAR) input::OnChar(unsigned(wp));
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SETFOCUS || msg == WM_ACTIVATE) input::NoteCapsLock();
    if ((msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || msg == WM_KILLFOCUS) input::ReleaseCursor();
    // the game's menus draw their own pointer (software_cursor.cpp), the F10 panel ImGui's: no Windows arrow
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        *result = TRUE;
        return true;
    }
#else
    (void)msg;
    (void)wp;
    (void)lp;
    (void)result;
#endif
    return false;
}

// the keyboard and mouse join the first controller, and port 0 is connected without one;
// the game sees no pad while the settings panel is open
void Pad(int port, input::Pad& pad) {
#ifdef _WIN32
    if (port != 0) return;
    input::SeenPad(pad);
    pad.connected = true;
    if (app::MenuOpen()) {
        app::PcOptionsPad(pad);
        pad = {};
        pad.connected = true;
        return;
    }
    // the keys press the default layout's controls; in play they follow the layout picked in the game's options
    ModernPad(pad);
    input::Pad keys;
    input::Keyboard(keys);
    input::Pad out = keys;
    if (input::Playing()) {
        constexpr uint16_t kStartBack = 0x30;
        out.buttons = keys.buttons & kStartBack;
        std::fill(std::begin(out.analog), std::end(out.analog), uint8_t(0));
        RemapToLayout(keys, out);
    }
    pad.buttons |= out.buttons;
    for (int i = 0; i < 8; i++) pad.analog[i] = std::max(pad.analog[i], out.analog[i]);
    if (out.lx) pad.lx = out.lx;
    if (out.ly) pad.ly = out.ly;
    if (out.rx) pad.rx = out.rx;
    if (out.ry) pad.ry = out.ry;
    // the sdrawkcaB cheat: both sticks (and the keys that move) turned round in play; the mouse in mouse_look.cpp
    if (input::Playing() && game::cheats::Unlocked() && game::cheats::On(game::cheats::kBackwards))
        for (int16_t* axis : {&pad.lx, &pad.ly, &pad.rx, &pad.ry}) *axis = int16_t(-std::max<int>(*axis, -32767));
    OptionsPad(pad);
#else
    if (port == 0) input::ApplyMenuTap(pad);  // the harness: the menu mouse's presses
    if (port == 0) OptionsPad(pad);
#endif
}

// no rumble while the keyboard and mouse play: a controller left on the desk would buzz at every shot
void Rumble(int port, uint16_t& left, uint16_t& right) {
#ifdef _WIN32
    if (port == 0 && input::KeyboardActive()) left = right = 0;
#else
    (void)port;
    (void)left;
    (void)right;
#endif
}

// the game window is up, on its thread: ImGui's backend sets the arrow on every frame, so the cursor is hidden
// for the thread outright and the menus draw their own
void Window(void* hwnd) {
#ifdef _WIN32
    ShowCursor(FALSE);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    input::StartMouse(hwnd);
    ::ui::paper::LoadFonts();  // before cursor::Init builds the font atlas
    cursor::Init(hwnd);
    ::ui::paper::Upload();  // the game's paper into the built atlas
#else
    (void)hwnd;
#endif
}

void Ui(double fps) {
    game::UpdateFrameBand();
#ifdef _WIN32
    app::DrawUi(fps);
    if (!app::MenuOpen() || app::PcOptionsOpen()) cursor::Draw();
#else
    (void)fps;
#endif
}

float ReadFloat(const toml::table& t, const char* key, float fallback) {
    return float(t[key].value_or(double(fallback)));
}

// what the options list cannot hold
void Load(const void* table) {
    const toml::table& t = *static_cast<const toml::table*>(table);
    Options& o = Opt();
    o.fov = ClampFov(o.fov);
    o.mouse_sensitivity = ReadFloat(t, "mouse_sensitivity", o.mouse_sensitivity);
    o.mouse_vertical_sensitivity = ReadFloat(t, "mouse_vertical_sensitivity", o.mouse_vertical_sensitivity);
    o.look_sensitivity_x = int32_t(t["look_sensitivity_x"].value_or(int64_t(-1)));
    o.look_sensitivity_y = int32_t(t["look_sensitivity_y"].value_or(int64_t(-1)));
    o.keys.clear();
    if (const toml::table* keys = t["keys"].as_table())
        for (auto&& [id, value] : *keys)
            if (auto v = value.value<std::string>()) o.keys[std::string(id.str())] = *v;
}

// each action's keys, the defaults written out so the file shows what can be bound
void Save(void* table) {
    toml::table& t = *static_cast<toml::table*>(table);
    const Options& o = Opt();
    t.insert_or_assign("mouse_sensitivity", double(o.mouse_sensitivity));
    t.insert_or_assign("mouse_vertical_sensitivity", double(o.mouse_vertical_sensitivity));
    if (o.look_sensitivity_x >= 0) t.insert_or_assign("look_sensitivity_x", int64_t(o.look_sensitivity_x));
    if (o.look_sensitivity_y >= 0) t.insert_or_assign("look_sensitivity_y", int64_t(o.look_sensitivity_y));
    size_t count;
    const input::Action* actions = input::Actions(&count);
    toml::table keys;
    for (size_t i = 0; i < count; i++) {
        auto it = o.keys.find(actions[i].id);
        keys.insert(actions[i].id, it != o.keys.end() ? it->second : std::string(actions[i].defaults));
    }
    t.insert_or_assign("keys", keys);
}

// after the settings load, before the window and boot
void Init() {
    input::LoadBindings();
    InitPrompts();
    // the frame rate is one of the menu's (30, 60, 120, 144, 240 or 0 for no limit), whatever an older settings file
    // says. (The game takes each frame's time from the clock, Com_Frame 0x2A70C0, as the engine it comes from does:
    // nothing of it counts frames. Movies keep their own 30: src/game/movies.cpp.)
    app::Settings& s = app::Get();
    constexpr uint32_t kCaps[] = {0, 30, 60, 120, 144, 240};
    if (std::find(std::begin(kCaps), std::end(kCaps), s.fps_cap) == std::end(kCaps)) s.fps_cap = 60;
}

}  // namespace

Options& Opt() {
    static Options o;
    return o;
}

}  // namespace game

// what the runtime asks of the game; strings must be set, hooks may stay null
const app::GameInfo app::kGame = {
    .title = L"Call of Duty 2: Big Red One",  // window and splash
    .short_title = L"Big Red One",  // message boxes
    .name = L"CoD2BRO",  // window classes, crash dumps, handover event
    .log = "cod2bro",  // log file prefix
    .settings = "settings.toml",  // settings file beside the exe
    .splash = L"splash",  // optional splash.png or splash.bmp beside the exe
    .missing = L"Put the game in the assets folder: its disc image (.iso), or default.xbe with its folders beside it.",
    .images = {{"default.xbe", "CoD2BRO"}},  // each recompiled xbe and its exe, for title launches
    .options = {{"fps_counter", &game::Opt().fps_counter, nullptr},
                {"fov", nullptr, &game::Opt().fov},
                {"invert_mouse", &game::Opt().invert_mouse, nullptr},
                {"invert_mouse_x", &game::Opt().invert_mouse_x, nullptr},
                {"qwerty_typewriter", &game::Opt().qwerty_typewriter, nullptr},
                {"button_prompts", nullptr, &game::Opt().button_prompts},
                {"modern_layout", &game::Opt().modern_layout, nullptr},
                {"brightness", nullptr, &game::Opt().brightness},
                {"view_distance", nullptr, &game::Opt().view_distance},
                {"aim_assist", &game::Opt().aim_assist, nullptr},
                {"hints", &game::Opt().hints, nullptr},
                {"hide_hud", &game::Opt().hide_hud, nullptr},
                {"walk_speed", nullptr, &game::Opt().walk_speed}},  // game settings kept in the settings file
    .load = game::Load,  // reads more from the settings file's toml::table: mouse sensitivities, [keys]
    .save = game::Save,  // writes more into it
    .init = game::Init,  // after the settings load, before the window and boot
    .loaded = nullptr,  // image in memory, before its entry point runs
    .window = game::Window,  // the game window is up
    .message = game::Message,  // window messages first; true takes one
    .ui = game::Ui,  // ImGui drawing each frame: the F10 panel, FPS counter and the menus' pointer
    .pad = game::Pad,  // changes a polled pad
    .rumble = game::Rumble,  // changes a rumble before it is set
};
