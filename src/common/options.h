#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>

#include "common/fov.h"

// settings kept in the settings file beside the exe, beyond the SDK's own (after MOHRS-XBERecomp's)
namespace game {

// The game's own options (its pause menu's Options and its Controller screen), which live in its profile: kept here
// too, as the game saves its profile only now and then, and set from the settings menu (src/game/game_options.cpp).
// -1: none kept yet, the profile's own.
enum GameOption {
    kGoSubtitles, kGoCrosshair, kGoTags, kGoTank, kGoAdsToggle, kGoInvert, kGoVibration, kGoVolume, kGoLayout,
    kGameOptions
};
// what a line shows before a profile has given its value
constexpr int32_t kGameOptionDefaults[kGameOptions] = {0, 1, 1, 0, 0, 0, 1, 50, 0};
constexpr int32_t kGameVolumeMax = 50, kLookSensitivityMax = 50, kLookSensitivityDefault = 25;

struct Options {
    std::array<int32_t, kGameOptions> game_options = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
    int32_t GameOption(int i) const { return game_options[size_t(i)] < 0 ? kGameOptionDefaults[i] : game_options[size_t(i)]; }
    // the game's volume as the settings menu has it, a percent (the game's own is 0 to 50, a half of it
    // rounded up); -1, or no longer the game's value: the game's value doubled
    int32_t master_volume = -1;
    int32_t MasterPercent() const {
        int32_t v = GameOption(kGoVolume);
        return master_volume >= 0 && (master_volume + 1) / 2 == v ? master_volume : v * 2;
    }
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
    uint32_t ambient_occlusion = 0;  // soft shade in corners and creases: 0 off, 1 SSAO, 2 GTAO (src/game/ambient_occlusion.cpp)
    bool ads_dof = false;        // all but the sight picture goes out of focus while aiming (src/game/fov.cpp)
    uint32_t shadow_distance = 0;  // how far round the camera shadows are cast: 0 the game's own, 1 three times, 2 six times
    bool sun_shadows = false;    // shadows fall away from the sun (src/game/shadows.cpp)
    bool gun_blur = false;       // the gun nearest the eye goes out of focus while aiming (src/game/fov.cpp)
    bool keep_bodies = false;    // dead soldiers stay where they fell (src/game/bodies.cpp)
    bool cutscene_bars = true;   // the black bars the level scripts put over their cutscenes (src/game/bodies.cpp)
    bool aim_assist = true;     // the game's sticky aim on a controller (never on the keyboard and mouse)
    bool hints = true;          // the level scripts' tutorial hints
    bool hide_hud = false;      // no HUD in a level
    bool aim_toggle = false;    // a press of the aim key brings the gun up, the next lowers it (mouse_keyboard.cpp)
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
// any thread: the game's own options, as Opt() has them now, go into the profile and are applied at the next frame
void ApplyGameOptions();

}  // namespace game
