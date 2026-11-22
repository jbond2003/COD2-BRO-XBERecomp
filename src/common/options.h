#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "common/fov.h"

// settings kept in the settings file beside the exe, beyond the SDK's own (after MOHRS-XBERecomp's)
namespace game {

struct Options {
    bool fps_counter = false;     // frames a second in the picture's top right corner
    uint32_t fov = kDefaultFov;   // cg_fov, horizontal degrees on a 4:3 picture
    // keyboard and mouse (src/common/mouse_keyboard.cpp)
    float mouse_sensitivity = 1.0f;  // horizontal, 0.11 degrees a count at 1, times the game's own aim sensitivity
    float mouse_vertical_sensitivity = 1.0f;
    bool invert_mouse = false;  // the Y axis
    bool invert_mouse_x = false;
    bool qwerty_typewriter = true;
    uint32_t button_prompts = 0;  // 0 follow the last device used, 1 always keys and mouse, 2 always the controller  // the profile name typewriter's letters in QWERTY order, not ABC
    uint32_t brightness = 100;  // percent: the picture's gamma, 100 the game's own (src/game/pc_settings.cpp)
    uint32_t view_distance = 1;  // times the game's own distances for models' detail (src/game/detail.cpp)
    bool ads_dof = false;        // all but the sight picture goes out of focus while aiming (src/game/fov.cpp)
    bool gun_blur = false;       // the gun nearest the eye goes out of focus while aiming (src/game/fov.cpp)
    bool keep_bodies = false;    // dead soldiers stay where they fell (src/game/bodies.cpp)
    bool cutscene_bars = true;   // the black bars the level scripts put over their cutscenes (src/game/bodies.cpp)
    bool aim_assist = true;     // the game's sticky aim on a controller (never on the keyboard and mouse)
    bool hints = true;          // the level scripts' tutorial hints
    bool hide_hud = false;      // no HUD in a level
    bool lean_toggle = false;   // a lean stays until its button is pressed again (src/game/lean.cpp), else while held
    // percent, 100 the game's own (src/game/audio.cpp)
    uint32_t music_volume = 100;       // the music in a mission
    uint32_t menu_music_volume = 100;  // the music in the menus
    uint32_t effects_volume = 100;     // sound effects, and the menus' own sounds
    uint32_t voice_volume = 100;       // dialogue
    bool modern_layout = true;  // the Modern controller configuration (src/game/pad_layout.cpp), else the game's own
    // the game's own look sensitivities (its Controller screen, pause Options), kept here as the game saves its profile
    // only now and then: -1 none kept yet, the profile's own (src/game/pad_layout.cpp)
    int32_t look_sensitivity_x = -1;
    int32_t look_sensitivity_y = -1;
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    // the profile these settings are (src/game/profile_settings.cpp): empty none yet, "*" a deleted one
    std::string settings_profile;
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};

Options& Opt();

}  // namespace game
