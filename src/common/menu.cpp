// the settings menu on F10 (drawn by modern_menu.cpp) and the PC Options pages
#include "common/menu.h"

float app::g_zoom = 1.0f;

#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <imgui.h>

#include "app/app.h"
#include "common/menu.h"
#include "common/cheats.h"
#include "common/console.h"
#include "common/modern_menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/paper_page.h"
#include "common/update_check.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_bindings_only{false};  // opened from PC Options: the key bindings alone

constexpr ImVec4 kHeading{0.93f, 0.66f, 0.26f, 1.0f};  // the FPS counter's brass

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2x", "4x", "8x"};
// Times the game's own distances for models' detail. v0.1.1 offered 1, 2, 4, 8: DetailLevel places those.
constexpr uint32_t kViewDistances[] = {1, 4, 8};
const char* const kViewDistanceNames[] = {"LOW (ORIGINAL)", "MEDIUM", "HIGH"};
constexpr int DetailLevel(uint32_t times) { return times <= 1 ? 0 : times <= 5 ? 1 : 2; }
const char* const kDumpFormatNames[] = {"PNG", "DDS"};
// 30 keeps the game's own limiter
constexpr uint32_t kFpsCaps[] = {30, 60, 120, 144, 240, 0};  // 0: no limit

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

// the index of the value nearest value, stepped first
template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < int(N); i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (step) {
        best = StepIndex(best, step, int(N));
        value = values[best];
    }
    return best;
}

// value clamped to lo to hi, stepped by inc
int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

// the window's monitor's modes, smallest first
const std::vector<Mode>& Modes() {
    static std::vector<Mode> modes = [] {
        std::vector<Mode> out;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY), &mi);
        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++) {
            if (dm.dmBitsPerPel < 32 || dm.dmPelsWidth < 640) continue;
            Mode m{uint32_t(dm.dmPelsWidth), uint32_t(dm.dmPelsHeight)};
            auto same = [&](const Mode& o) { return o.width == m.width && o.height == m.height; };
            if (std::none_of(out.begin(), out.end(), same)) out.push_back(m);
        }
        std::sort(out.begin(), out.end(), [](const Mode& a, const Mode& b) {
            return a.width != b.width ? a.width < b.width : a.height < b.height;
        });
        return out;
    }();
    return modes;
}

enum RowId { kSection, kDisplayMode, kResolution, kRenderResolution, kFov, kMaxFps, kVsync, kFpsCounter, kAniso,
             kTextureReplacement, kTextureDumping, kDumpFormat, kMouseX, kMouseY, kInvertY, kInvertX, kQwerty, kPrompts,
             kBrightness, kAimAssist, kHints, kHideHud, kViewDistance, kKeepBodies, kCutsceneBars, kLeanToggle,
             kMusicVolume, kMenuMusicVolume, kEffectsVolume, kVoiceVolume, kMsaa, kSmaa, kAmbientOcclusion, kAdsDof, kGunBlur, kAimToggle, kPreset, kPlayerBody,
             // the game's own options (options.h's GameOption, src/game/game_options.cpp)
             kGSubtitles, kGCrosshair, kGTags, kGTank, kGAdsToggle, kGInvert, kGVibration, kGVolume, kGLayout,
             kGSensX, kGSensY };
// a row of the game's own options: its place in Options::game_options, or -1
int GameOptionOf(RowId id) {
    return id >= kGSubtitles && id <= kGLayout ? int(id) - int(kGSubtitles) : -1;
}
const char* const kLayoutNames[] = {"A", "B", "C", "D", "MODERN"};

struct Row {
    RowId id;
    const char* label;
};
constexpr Row kRows[] = {
    {kSection, "DISPLAY"},
    {kDisplayMode, "DISPLAY MODE"},
    {kResolution, "RESOLUTION"},
    {kRenderResolution, "RENDER RESOLUTION"},
    {kFov, "FIELD OF VIEW"},
    {kMaxFps, "MAX FRAME RATE"},
    {kVsync, "VSYNC"},
    {kFpsCounter, "FPS COUNTER"},
    {kBrightness, "BRIGHTNESS"},
    {kCutsceneBars, "CUTSCENE BLACK BARS"},
    {kSection, "GRAPHICS"},
    {kAniso, "ANISOTROPIC FILTERING"},
    {kViewDistance, "LEVEL OF DETAIL"},
    {kKeepBodies, "BODIES STAY"},
    {kSection, "TEXTURES"},
    {kTextureReplacement, "TEXTURE REPLACEMENT"},
    {kTextureDumping, "TEXTURE DUMPING"},
    {kDumpFormat, "DUMP FORMAT"},
    {kSection, "MOUSE"},
    {kMouseX, "HORIZONTAL SENSITIVITY"},
    {kMouseY, "VERTICAL SENSITIVITY"},
    {kInvertY, "INVERT VERTICAL LOOK"},
    {kInvertX, "INVERT HORIZONTAL LOOK"},
    {kSection, "MENUS"},
    {kQwerty, "PROFILE NAME KEYS"},
    {kPrompts, "BUTTON PROMPTS"},
    {kSection, "GAMEPLAY"},
    {kAimAssist, "CONTROLLER AIM ASSIST"},
    {kHints, "TUTORIAL HINTS"},
    {kHideHud, "HIDE THE HUD"},
    {kLeanToggle, "LEANING"},
};

bool DisplayRow(RowId id) { return id == kDisplayMode || id == kResolution; }

// display mode and resolution wait for Apply, the rest take effect at once
Settings g_held;
bool g_held_set = false;

Settings& Shown(RowId id) { return g_held_set && DisplayRow(id) ? g_held : Get(); }

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

// the game's own options RowText reads and steps: the live ones, or a held copy (PC Options holds a page's changes
// until Apply)
game::Options* g_row_options = nullptr;
game::Options& O() { return g_row_options ? *g_row_options : game::Opt(); }

// what a row shows, stepped first when step is not zero
std::string RowText(RowId id, int step, Settings& s) {
    char text[64] = "";
    switch (id) {
        case kDisplayMode: {
            static const char* const kModes[] = {"FULLSCREEN", "BORDERLESS", "WINDOWED"};
            int count = int(std::size(kModes));
            if (step) s.display_mode = DisplayMode(StepIndex(int(s.display_mode), step, count));
            std::snprintf(text, sizeof(text), "%s", kModes[std::min(int(s.display_mode), count - 1)]);
            break;
        }
        case kResolution: {
            const std::vector<Mode>& modes = Modes();
            Mode desktop = Desktop();
            // a borderless window is the desktop's size: nothing to pick (the size kept is for the other modes)
            if (s.display_mode == DisplayMode::Borderless) {
                std::snprintf(text, sizeof(text), "%u x %u", desktop.width, desktop.height);
                break;
            }
            Mode m = s.width && s.height ? Mode{s.width, s.height} : desktop;
            int i = 0;
            for (size_t k = 0; k < modes.size(); k++)
                if (modes[k].width == m.width && modes[k].height == m.height) i = int(k);
            if (step && !modes.empty()) {
                m = modes[size_t(StepIndex(i, step, int(modes.size())))];
                // the desktop's own mode is kept as 0, which follows the desktop
                bool native = m.width == desktop.width && m.height == desktop.height;
                s.width = native ? 0 : m.width;
                s.height = native ? 0 : m.height;
            }
            std::snprintf(text, sizeof(text), "%u x %u", m.width, m.height);
            break;
        }
        case kRenderResolution: {
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 1, step));
            Mode out = Output();
            std::snprintf(text, sizeof(text), "%u%% (%ux%u)", s.render_percent, out.width * s.render_percent / 100,
                          out.height * s.render_percent / 100);
            break;
        }
        case kMaxFps: {
            // (the nearest value's place; no limit is 0, which only 0 itself is nearest to)
            uint32_t at = s.fps_cap ? std::max<uint32_t>(s.fps_cap, 16) : 0;
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, at, step)];
            if (step) s.fps_cap = cap;
            else cap = s.fps_cap;  // a rate of the console's com_maxfps is shown as it is
            std::snprintf(text, sizeof(text), cap == 30 ? "30 (ORIGINAL)" : cap ? "%u" : "UNLIMITED", cap);
            break;
        }
        case kFov:
        {
            uint32_t& fov = O().fov;
            if (step) fov = uint32_t(StepRange(int(fov), int(game::kMinFov), int(game::kMaxFov), 1, step));
            std::snprintf(text, sizeof(text), fov == game::kDefaultFov ? "%u (DEFAULT)" : "%u", fov);
        }
            break;
        case kVsync:
            if (step) s.vsync = !s.vsync;
            return OnOff(s.vsync);
        case kFpsCounter:
            if (step) O().fps_counter = !O().fps_counter;
            return OnOff(O().fps_counter);
        case kBrightness: {
            uint32_t& b = O().brightness;
            if (step) b = uint32_t(StepRange(int(b), 50, 200, 1, step));
            std::snprintf(text, sizeof(text), b == 100 ? "%u (DEFAULT)" : "%u", b);
            break;
        }
        case kAimAssist:
            if (step) O().aim_assist = !O().aim_assist;
            return OnOff(O().aim_assist);
        case kHints:
            if (step) O().hints = !O().hints;
            return OnOff(O().hints);
        case kHideHud:
            if (step) O().hide_hud = !O().hide_hud;
            return OnOff(O().hide_hud);
        case kGSubtitles:
        case kGCrosshair:
        case kGTags:
        case kGTank:
        case kGInvert:
        case kGVibration:
        case kGAdsToggle: {
            int i = GameOptionOf(id);
            bool on = O().GameOption(i) != 0;
            if (step) on = !on, O().game_options[size_t(i)] = on;
            if (id == kGAdsToggle) return on ? "TOGGLE" : "HOLD";
            return OnOff(on);
        }
        case kGVolume: {
            // a percent; the game's own is a half of it
            int v = O().MasterPercent();
            if (step) {
                v = StepRange(v, 0, 100, 1, step);
                O().master_volume = v;
                O().game_options[game::kGoVolume] = (v + 1) / 2;
            }
            std::snprintf(text, sizeof(text), "%d", v);
            break;
        }
        case kGSensX:
        case kGSensY: {
            int32_t& kept = id == kGSensX ? O().look_sensitivity_x : O().look_sensitivity_y;
            int v = kept < 0 ? game::kLookSensitivityDefault : kept;
            if (step) v = StepRange(v, 0, game::kLookSensitivityMax, 1, step), kept = v;
            std::snprintf(text, sizeof(text), "%d", v);
            break;
        }
        case kGLayout: {
            int at = O().modern_layout ? 4 : std::clamp(O().GameOption(game::kGoLayout), 0, 3);
            if (step) {
                at = (at + (step > 0 ? 1 : 4)) % 5;
                O().modern_layout = at == 4;
                if (at < 4) O().game_options[game::kGoLayout] = at;
            }
            return kLayoutNames[at];
        }
        case kAimToggle:
            if (step) O().aim_toggle = !O().aim_toggle;
            return O().aim_toggle ? "TOGGLE" : "HOLD";
        case kLeanToggle:
            if (step) O().lean_toggle = !O().lean_toggle;
            return O().lean_toggle ? "TOGGLE" : "HOLD (ORIGINAL)";
        case kPreset: {
            // Original: the game as it shipped, every addition of the port's on this tab off. Remastered: them on,
            // smoothing by SMAA. The settings themselves say which it is: any of them changed by hand shows Custom.
            // A step goes to the other one.
            game::Options& o = O();
            bool original = !o.ads_dof && !o.gun_blur && !o.ambient_occlusion && !o.keep_bodies &&
                            DetailLevel(o.view_distance) == 0 && s.aniso == 0 && s.msaa == 0 && !s.smaa;
            bool remastered = o.ads_dof && o.gun_blur && o.ambient_occlusion == 1 && o.keep_bodies &&
                              DetailLevel(o.view_distance) == 2 && s.aniso == 16 && s.msaa == 0 && s.smaa;
            if (step) {
                bool to = original ? true : remastered ? false : step > 0;  // to Remastered
                o.ads_dof = o.gun_blur = o.keep_bodies = s.smaa = to;
                o.ambient_occlusion = to ? 1 : 0;
                o.view_distance = to ? 8 : 1;
                s.aniso = to ? 16 : 0;
                s.msaa = 0;
                return to ? "REMASTERED" : "ORIGINAL";
            }
            return original ? "ORIGINAL" : remastered ? "REMASTERED" : "CUSTOM";
        }
        case kAmbientOcclusion: {
            static const char* const kNames[] = {"OFF", "SSAO", "GTAO"};
            uint32_t& v = O().ambient_occlusion;
            if (v > 2) v = 0;
            if (step) v = (v + (step > 0 ? 1 : 2)) % 3;
            return kNames[v];
        }
        case kPlayerBody:
            if (step) O().player_body = !O().player_body;
            return OnOff(O().player_body);
        case kGunBlur:
            if (step) O().gun_blur = !O().gun_blur;
            return OnOff(O().gun_blur);
        case kAdsDof:
            if (step) O().ads_dof = !O().ads_dof;
            return OnOff(O().ads_dof);
        case kKeepBodies:
            if (step) O().keep_bodies = !O().keep_bodies;
            return OnOff(O().keep_bodies);
        case kCutsceneBars:
            if (step) O().cutscene_bars = !O().cutscene_bars;
            return OnOff(O().cutscene_bars);
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            break;
        case kMsaa:
            std::snprintf(text, sizeof(text), "%s", kMsaaNames[StepTable(kMsaaValues, s.msaa, step)]);
            break;
        case kSmaa:
            if (step) s.smaa = !s.smaa;
            return OnOff(s.smaa);
        case kViewDistance:
        {
            int level = DetailLevel(O().view_distance);
            if (step) {
                level = StepIndex(level, step, int(std::size(kViewDistances)));
                O().view_distance = kViewDistances[level];
            }
            std::snprintf(text, sizeof(text), "%s", kViewDistanceNames[level]);
        }
            break;
        case kTextureReplacement:
            if (step) s.custom_textures = !s.custom_textures;
            return OnOff(s.custom_textures);
        case kTextureDumping:
            if (step) s.dump_textures = !s.dump_textures;
            return OnOff(s.dump_textures);
        case kDumpFormat:
            if (step) s.texture_dump_format = s.texture_dump_format ? 0 : 1;
            std::snprintf(text, sizeof(text), "%s", kDumpFormatNames[std::min(s.texture_dump_format, 1u)]);
            break;
        case kMouseX:
        case kMouseY: {
            float& v = id == kMouseX ? O().mouse_sensitivity : O().mouse_vertical_sensitivity;
            // shown as a percent of the game's own (m_sensitivity 1.0 is 100)
            if (step) v = float(StepRange(int(v * 100.0f + 0.5f), 10, 500, 1, step)) / 100.0f;
            std::snprintf(text, sizeof(text), v == 1.0f ? "%d (DEFAULT)" : "%d", int(v * 100.0f + 0.5f));
            break;
        }
        case kInvertY:
            if (step) O().invert_mouse = !O().invert_mouse;
            return OnOff(O().invert_mouse);
        case kInvertX:
            if (step) O().invert_mouse_x = !O().invert_mouse_x;
            return OnOff(O().invert_mouse_x);
        case kMusicVolume:
        case kMenuMusicVolume:
        case kEffectsVolume:
        case kVoiceVolume: {
            uint32_t& v = id == kMusicVolume ? O().music_volume : id == kMenuMusicVolume ? O().menu_music_volume :
                          id == kEffectsVolume ? O().effects_volume : O().voice_volume;
            if (step) v = uint32_t(StepRange(int(v), 0, 100, 1, step));
            std::snprintf(text, sizeof(text), "%u", v);
            break;
        }
        case kQwerty:
            if (step) O().qwerty_typewriter = !O().qwerty_typewriter;
            return O().qwerty_typewriter ? "QWERTY" : "ABC (ORIGINAL)";
        case kPrompts: {
            static constexpr const char* kModes[] = {"AUTOMATIC", "KEYBOARD AND MOUSE", "CONTROLLER"};
            uint32_t& m = O().button_prompts;
            if (m > 2) m = 0;
            if (step) m = uint32_t((int(m) + step + 3) % 3);
            return kModes[m];
        }
        default:
            break;
    }
    return text;
}

void Apply() {
    if (!g_held_set) return;
    Settings& s = Get();
    s.display_mode = g_held.display_mode;
    s.width = g_held.width;
    s.height = g_held.height;
    g_held_set = false;
    ApplyDisplay();
    Save();
}

// the settings as they come into s and o, a part of them (a PC Options page resets its own) or all
constexpr int kDisplayPart = 1, kGraphicsPart = 2, kMousePart = 4, kKeysPart = 8, kGameplayPart = 16, kAudioPart = 32,
              kEveryPart = 63;
void SetDefaults(Settings& s, game::Options& o, int parts) {
    Settings d;
    if (parts & kDisplayPart) {
        s.display_mode = d.display_mode;
        s.width = d.width;
        s.height = d.height;
        s.render_percent = d.render_percent;
        s.fps_cap = 60;
        s.vsync = false;
        o.fps_counter = false;
        o.fov = game::kDefaultFov;
        o.brightness = 100;
        o.cutscene_bars = true;
    }
    if (parts & kGameplayPart) {
        o.aim_assist = true;
        o.hints = true;
        o.hide_hud = false;
        o.lean_toggle = false;
        for (int i : {game::kGoSubtitles, game::kGoCrosshair, game::kGoTags, game::kGoTank}) o.game_options[size_t(i)] = game::kGameOptionDefaults[i];
        o.aim_toggle = false;
    }
    if (parts & kAudioPart) {
        o.music_volume = 100;
        o.menu_music_volume = 100;
        o.effects_volume = 100;
        o.voice_volume = 100;
    }
    if (parts & kGraphicsPart) {
        s.aniso = d.aniso;
        s.msaa = 0;
        s.smaa = false;
        o.view_distance = 1;
        o.keep_bodies = false;
        o.ambient_occlusion = 0;
        o.ads_dof = false;
        o.gun_blur = false;
        o.player_body = false;
        s.custom_textures = d.custom_textures;
        s.dump_textures = d.dump_textures;
        s.texture_dump_format = d.texture_dump_format;
    }
    if (parts & kMousePart) {
        // the controller's lines are on the same tab
        for (int i : {game::kGoInvert, game::kGoVibration, game::kGoAdsToggle}) o.game_options[size_t(i)] = game::kGameOptionDefaults[i];
        o.look_sensitivity_x = o.look_sensitivity_y = game::kLookSensitivityDefault;
        o.modern_layout = true;
        o.mouse_sensitivity = 1.0f;
        o.mouse_vertical_sensitivity = 1.0f;
        o.invert_mouse = false;
        o.invert_mouse_x = false;
    }
    if (parts & kKeysPart) {
        o.qwerty_typewriter = true;
        o.button_prompts = 0;
    }
}

// what the pages' rows change, from one set to another
void CopyRows(Settings& s, game::Options& o, const Settings& from, const game::Options& from_o) {
    s.display_mode = from.display_mode;
    s.width = from.width;
    s.height = from.height;
    s.render_percent = from.render_percent;
    s.fps_cap = from.fps_cap;
    s.vsync = from.vsync;
    s.aniso = from.aniso;
    s.msaa = from.msaa;
    s.smaa = from.smaa;
    s.custom_textures = from.custom_textures;
    s.dump_textures = from.dump_textures;
    s.texture_dump_format = from.texture_dump_format;
    o.fps_counter = from_o.fps_counter;
    o.fov = from_o.fov;
    o.mouse_sensitivity = from_o.mouse_sensitivity;
    o.mouse_vertical_sensitivity = from_o.mouse_vertical_sensitivity;
    o.invert_mouse = from_o.invert_mouse;
    o.invert_mouse_x = from_o.invert_mouse_x;
    o.qwerty_typewriter = from_o.qwerty_typewriter;
    o.button_prompts = from_o.button_prompts;
    o.brightness = from_o.brightness;
    o.aim_assist = from_o.aim_assist;
    o.hints = from_o.hints;
    o.hide_hud = from_o.hide_hud;
    o.lean_toggle = from_o.lean_toggle;
    o.game_options = from_o.game_options;
    o.master_volume = from_o.master_volume;
    o.look_sensitivity_x = from_o.look_sensitivity_x;
    o.look_sensitivity_y = from_o.look_sensitivity_y;
    o.modern_layout = from_o.modern_layout;
    o.aim_toggle = from_o.aim_toggle;
    o.view_distance = from_o.view_distance;
    o.keep_bodies = from_o.keep_bodies;
    o.ambient_occlusion = from_o.ambient_occlusion;
    o.ads_dof = from_o.ads_dof;
    o.gun_blur = from_o.gun_blur;
    o.player_body = from_o.player_body;
    o.cutscene_bars = from_o.cutscene_bars;
    o.music_volume = from_o.music_volume;
    o.menu_music_volume = from_o.menu_music_volume;
    o.effects_volume = from_o.effects_volume;
    o.voice_volume = from_o.voice_volume;
}

// one set of the rows' values differs from another
bool RowsDiffer(const Settings& a, const game::Options& c, const Settings& b, const game::Options& d) {
    return a.display_mode != b.display_mode || a.width != b.width || a.height != b.height ||
           a.render_percent != b.render_percent || a.fps_cap != b.fps_cap || a.vsync != b.vsync || a.aniso != b.aniso ||
           a.msaa != b.msaa || a.smaa != b.smaa ||
           a.custom_textures != b.custom_textures || a.dump_textures != b.dump_textures ||
           a.texture_dump_format != b.texture_dump_format || c.fps_counter != d.fps_counter || c.fov != d.fov ||
           c.mouse_sensitivity != d.mouse_sensitivity || c.mouse_vertical_sensitivity != d.mouse_vertical_sensitivity ||
           c.invert_mouse != d.invert_mouse || c.invert_mouse_x != d.invert_mouse_x ||
           c.qwerty_typewriter != d.qwerty_typewriter || c.button_prompts != d.button_prompts ||
           c.brightness != d.brightness || c.aim_assist != d.aim_assist || c.hints != d.hints ||
           c.hide_hud != d.hide_hud || c.lean_toggle != d.lean_toggle || c.game_options != d.game_options || c.look_sensitivity_x != d.look_sensitivity_x ||
           c.look_sensitivity_y != d.look_sensitivity_y || c.modern_layout != d.modern_layout || c.aim_toggle != d.aim_toggle || c.view_distance != d.view_distance || c.keep_bodies != d.keep_bodies || c.player_body != d.player_body || c.ambient_occlusion != d.ambient_occlusion || c.ads_dof != d.ads_dof || c.gun_blur != d.gun_blur ||
           c.cutscene_bars != d.cutscene_bars || c.music_volume != d.music_volume ||
           c.menu_music_volume != d.menu_music_volume || c.effects_volume != d.effects_volume ||
           c.voice_volume != d.voice_volume;
}

// The F10 panel's changes, every row's, are held until its Apply (his ask: nothing changes before Apply, as in PC
// Options); closing the panel drops them. Apply is lit while something held differs from what is in use.
Settings g_panel_settings;
game::Options g_panel_options;
bool g_panel_pending = false;

void HoldPanel() {
    if (g_panel_pending) return;
    g_panel_settings = Get();
    g_panel_options = game::Opt();
    g_panel_pending = true;
}

// the cheats ticked in the panel, held like the settings until Apply
struct HeldCheats {
    bool pending = false;
    bool on[game::cheats::kToggles] = {};
    bool ammo = false;
    bool heal = false;  // Full health, once, on Apply
} g_held_cheats;

void HoldCheats() {
    if (g_held_cheats.pending) return;
    g_held_cheats = {};
    g_held_cheats.pending = true;
    for (int t = 0; t < game::cheats::kToggles; t++) g_held_cheats.on[t] = game::cheats::On(game::cheats::Toggle(t));
    g_held_cheats.ammo = game::cheats::UnlimitedAmmo();
}

bool CheatsChanged() {
    const HeldCheats& h = g_held_cheats;
    if (!h.pending) return false;
    for (int t = 0; t < game::cheats::kToggles; t++)
        if (h.on[t] != game::cheats::On(game::cheats::Toggle(t))) return true;
    return h.heal || h.ammo != game::cheats::UnlimitedAmmo();
}

bool PanelChanged() {
    return CheatsChanged() || (g_panel_pending && RowsDiffer(g_panel_settings, g_panel_options, Get(), game::Opt()));
}

// a row's words in the panel, stepped first when step is not zero: the held values once anything is held
std::string PanelText(RowId id, int step) {
    if (step) HoldPanel();
    g_row_options = g_panel_pending ? &g_panel_options : nullptr;
    std::string text = RowText(id, step, g_panel_pending ? g_panel_settings : Get());
    g_row_options = nullptr;
    return text;
}

void ApplyPanel() {
    if (g_held_cheats.pending) {
        for (int t = 0; t < game::cheats::kToggles; t++) game::cheats::Set(game::cheats::Toggle(t), g_held_cheats.on[t]);
        game::cheats::SetUnlimitedAmmo(g_held_cheats.ammo);
        if (g_held_cheats.heal) game::cheats::FullHealth();
        g_held_cheats = {};
    }
    if (!g_panel_pending) return;
    g_panel_pending = false;
    Settings& s = Get();
    bool display = s.display_mode != g_panel_settings.display_mode || s.width != g_panel_settings.width ||
                   s.height != g_panel_settings.height;
    CopyRows(s, game::Opt(), g_panel_settings, g_panel_options);
    if (display) ApplyDisplay();
    Save();
    game::ApplyGameOptions();  // the game's own, into its profile at the next frame
}

// the F10 panel's Defaults: held like any other change, until Apply
void Defaults() {
    HoldPanel();
    SetDefaults(g_panel_settings, g_panel_options, kEveryPart);
}

// a binding box waiting for a key: the action and slot, the keys let go of first
struct Capture {
    int action = -1;
    int slot = 0;
    bool released = false;
};
Capture g_capture;

// the key a waiting box takes: none until every key is up, then the first one down; Escape and Backspace (the
// menus' back keys) cancel
void UpdateCapture() {
    if (g_capture.action < 0) return;
    int vk = input::PressedKey();
    if (!g_capture.released) {
        g_capture.released = vk == 0;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE && vk != VK_BACK) input::Bind(size_t(g_capture.action), vk, g_capture.slot);
    g_capture = {};
}

// ---- the PC Options page: the same settings on one of the game's own paper option pages (paper_page.h) ----

std::atomic<bool> g_paper{false};
std::atomic<uint32_t> g_paper_pad{0};  // the controller as kPad* bits, from the game thread's poll

constexpr uint32_t kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadA = 0x100, kPadB = 0x200,
                   kPadX = 0x400, kPadY = 0x800, kPadPrev = 0x1000, kPadNext = 0x2000;

struct PaperState {
    int selected = -1;  // a line index
    float scroll = 0;
    bool follow = true;  // keep the selection in view: the keys moved it
    ImVec2 mouse{-1, -1};
    uint32_t pad_prev = 0;
    double pad_repeat = 0;
    std::vector<ui::paper::Hit> hits;
} g_page;

// what each line of the page is
struct PaperRow {
    enum Kind { kSetting, kBind, kResetAll, kApplyExit } kind;
    RowId row = kSection;
    size_t action = 0;
};

// the page's words in the game's own case: "Field of view", "On"; short names stay capitals
// a name with a capital on every word ("Lean Left"), for the settings menu's key list
std::string TitleCase(const std::string& text) {
    static constexpr const char* kKeep[] = {"HUD", "FPS", "FOV", "D-PAD"};
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '/') j++;
        std::string word = text.substr(i, j - i);
        if (std::none_of(std::begin(kKeep), std::end(kKeep), [&](const char* k) { return word == k; }))
            for (size_t k = 1; k < word.size(); k++) word[k] = char(std::tolower(static_cast<unsigned char>(word[k])));
        out += word;
        if (j < text.size()) out += text[j];
        i = j + 1;
    }
    return out;
}

std::string Sentence(const std::string& text) {
    static constexpr const char* kKeep[] = {"PNG", "DDS", "QWERTY", "ABC", "FPS", "FOV", "D-PAD", "SSAO", "GTAO"};
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '(' && text[j] != ')' && text[j] != '/') j++;
        std::string word = text.substr(i, j - i);
        bool keep = std::any_of(std::begin(kKeep), std::end(kKeep), [&](const char* k) { return word == k; });
        if (!keep)
            for (char& c : word) c = char(std::tolower(static_cast<unsigned char>(c)));
        out += word;
        if (j < text.size()) out += text[j];
        i = j + 1;
    }
    // (a number first: a size, "1920 x 1080", keeps its small x)
    for (char& c : out) {
        if (std::isdigit(static_cast<unsigned char>(c))) break;
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = char(std::toupper(static_cast<unsigned char>(c)));
            break;
        }
    }
    return out;
}

void BuildPage(std::vector<ui::paper::Line>& lines, std::vector<PaperRow>& rows) {
    using ui::paper::Line;
    lines.clear();
    rows.clear();
    for (const Row& r : kRows) {
        Line l;
        if (r.id == kSection) {
            l.kind = Line::kHeading;
            l.label = Sentence(r.label);
        } else {
            l.kind = Line::kOption;
            l.label = Sentence(r.label);
            l.value = Sentence(RowText(r.id, 0, Shown(r.id)));
        }
        lines.push_back(l);
        rows.push_back({PaperRow::kSetting, r.id});
    }
    lines.push_back({Line::kHeading, "Key bindings"});
    rows.push_back({PaperRow::kSetting, kSection});
    size_t count = 0;
    input::Actions(&count);
    for (size_t i = 0; i < count; i++) {
        Line l;
        l.kind = Line::kBinding;
        l.label = Sentence(input::Label(i));
        l.value = input::Binding(i, 0);
        l.value2 = input::Binding(i, 1);
        lines.push_back(l);
        rows.push_back({PaperRow::kBind, kSection, i});
    }
    lines.push_back({Line::kAction, "Reset to Defaults"});
    rows.push_back({PaperRow::kResetAll});
    lines.push_back({Line::kAction, "Apply and Exit"});
    rows.push_back({PaperRow::kApplyExit});
}

int g_slot = 0;  // the key box the selection is on, on a binding line

void StepSetting(RowId id, int step) {
    if (DisplayRow(id)) {
        if (!g_held_set) g_held = Get();
        g_held_set = true;
        RowText(id, step, g_held);
    } else {
        RowText(id, step, Get());
        Save();
    }
}

void ClosePaper() {
    if (g_paper.load(std::memory_order_relaxed) && MenuOpen()) ToggleMenu();
}

// a line's job: step an option, wait for a key, or the page's actions
void Activate(const PaperRow& r, int step) {
    switch (r.kind) {
        case PaperRow::kSetting:
            if (r.row != kSection) StepSetting(r.row, step ? step : 1);
            break;
        case PaperRow::kBind:
            g_capture = {int(r.action), g_slot, false};
            break;
        case PaperRow::kResetAll:
            Defaults();
            ApplyPanel();  // (this page applies its other lines at once too)
            g_capture = {};
            input::ResetBindings();
            break;
        case PaperRow::kApplyExit:
            Apply();
            Save();
            ClosePaper();
            break;
    }
}

bool Inside(ImVec2 p, ImVec2 min, ImVec2 max) {
    return max.x > min.x && p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
}

// the next line the selection can stop on, from line index from going dir (wraps)
int NextLine(const std::vector<ui::paper::Line>& lines, int from, int dir) {
    int n = int(lines.size());
    for (int k = 1; k <= n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (lines[size_t(i)].kind != ui::paper::Line::kHeading) return i;
    }
    return from;
}

void DrawPaper() {
    std::vector<ui::paper::Line> lines;
    std::vector<PaperRow> rows;
    BuildPage(lines, rows);
    PaperState& p = g_page;
    if (p.selected < 0 || p.selected >= int(lines.size()) || lines[size_t(p.selected)].kind == ui::paper::Line::kHeading)
        p.selected = NextLine(lines, -1, 1), p.follow = true;

    bool was_capturing = g_capture.action >= 0;
    UpdateCapture();
    bool capturing = g_capture.action >= 0;

    // the controller: edges, and the dpad repeating while held
    uint32_t pad = g_paper_pad.load(std::memory_order_relaxed);
    uint32_t pressed = pad & ~p.pad_prev;
    double now = ImGui::GetTime();
    constexpr uint32_t kDirs = kPadUp | kPadDown | kPadLeft | kPadRight;
    if (pressed & kDirs) p.pad_repeat = now + 0.4;
    else if ((pad & kDirs) && now >= p.pad_repeat) {
        pressed |= pad & kDirs;
        p.pad_repeat = now + 0.12;
    }
    p.pad_prev = pad;

    // capture takes every key until it ends; a key that ended it does nothing else this frame
    if (!capturing && !was_capturing) {
        auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        int move = 0, step = 0;
        if (key(ImGuiKey_UpArrow) || (pressed & kPadUp)) move = -1;
        if (key(ImGuiKey_DownArrow) || (pressed & kPadDown)) move = 1;
        if (key(ImGuiKey_LeftArrow) || (pressed & kPadLeft)) step = -1;
        if (key(ImGuiKey_RightArrow) || (pressed & kPadRight)) step = 1;
        bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                     (pressed & kPadA);
        bool back = ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || (pressed & kPadB);
        bool clear = ImGui::IsKeyPressed(ImGuiKey_Delete, false) || (pressed & kPadX);
        if (move) {
            p.selected = NextLine(lines, p.selected, move);
            p.follow = true;
        }
        const PaperRow& r = rows[size_t(p.selected)];
        if (step) {
            if (r.kind == PaperRow::kBind) g_slot = step < 0 ? 0 : 1;
            else if (r.kind == PaperRow::kSetting) Activate(r, step);
        }
        if (enter) Activate(r, 0);
        if (clear && r.kind == PaperRow::kBind) input::Bind(r.action, 0, g_slot);
        if (back) {
            ClosePaper();
            return;
        }

        // the mouse: hovering selects, a click does the line's job, the wheel scrolls
        const ImGuiIO& io = ImGui::GetIO();
        ImVec2 m = io.MousePos;
        bool moved = m.x != p.mouse.x || m.y != p.mouse.y;
        p.mouse = m;
        int over = -1;
        for (size_t i = 0; i < p.hits.size() && i < lines.size(); i++)
            if (Inside(m, p.hits[i].min, p.hits[i].max)) over = int(i);
        bool left = ImGui::IsMouseClicked(ImGuiMouseButton_Left), right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (over >= 0 && (moved || left || right)) {
            const ui::paper::Hit& h = p.hits[size_t(over)];
            const PaperRow& o = rows[size_t(over)];
            p.selected = over;
            if (o.kind == PaperRow::kBind) {
                if (Inside(m, h.left_min, h.left_max)) g_slot = 0;
                if (Inside(m, h.right_min, h.right_max)) g_slot = 1;
            }
            bool on_box = Inside(m, h.left_min, h.left_max) || Inside(m, h.right_min, h.right_max);
            if (left) {
                if (o.kind == PaperRow::kSetting)
                    Activate(o, Inside(m, h.left_min, h.left_max) ? -1 : 1);
                else if (o.kind != PaperRow::kBind || on_box)
                    Activate(o, 0);
            }
            if (right) {
                if (o.kind == PaperRow::kBind && on_box) input::Bind(o.action, 0, g_slot);
                if (o.kind == PaperRow::kSetting) Activate(o, -1);
            }
        }
        if (io.MouseWheel != 0) {
            p.scroll -= io.MouseWheel * 2.0f;
            p.follow = false;
        }
        BuildPage(lines, rows);  // the values as they are now
    }
    lines[size_t(p.selected)].slot = g_slot;
    lines[size_t(p.selected)].waiting = capturing;
    const char* note = capturing ? "Press a key for it, Escape to keep the old one" :
                       g_held_set ? "Display changes take effect on Apply and Exit" :
                       "Arrows or the mouse to change, Delete or right click clears a key, Escape goes back";
    ui::paper::Draw("PC OPTIONS", lines, p.selected, p.scroll, p.follow, note, p.hits);
    p.follow = false;
}

// ---- the settings menu on F10: the settings by tab on the game's sheet (modern_menu.h) ----

constexpr int kDisplayTab = 0, kGraphicsTab = 1, kGameplayTab = 2, kAudioTab = 3, kControlsTab = 4,
              kControllerTab = 5, kCheatsTab = 6, kTabs = 7;
const char* const kTabNames[kTabs] = {"Display", "Graphics", "Gameplay", "Audio", "Controls", "Controller", "Cheats"};

// a tab's settings: kSection is a heading. The note is what the slip at the right says, the variable the console's
// name for the same setting (src/game/console.cpp)
struct TabRow {
    int tab;
    RowId id;
    const char* label;
    const char* note;
    const char* variable;
};
constexpr TabRow kTabRows[] = {
    {kDisplayTab, kSection, "Screen", "", ""},
    {kDisplayTab, kDisplayMode, "Display Mode", "Fullscreen, a borderless window the size of the desktop, or a window.", ""},
    {kDisplayTab, kResolution, "Resolution", "The size of the game's window or screen mode.", ""},
    {kDisplayTab, kVsync, "VSync", "Each frame waits for the monitor. No tearing, a little more delay on the mouse.", "r_swapInterval"},
    {kDisplayTab, kMaxFps, "Max Frame Rate", "The most frames a second.", "com_maxfps"},
    {kDisplayTab, kSection, "Picture", "", ""},
    {kDisplayTab, kFov, "Field of View", "How wide you see. Higher shows more of the battlefield and makes things look further away.", "cg_fov"},
    {kDisplayTab, kBrightness, "Brightness", "Lightens or darkens the whole picture.", "r_gamma"},
    {kDisplayTab, kCutsceneBars, "Cutscene Black Bars", "The black bars the game puts over its cutscenes.", "cg_cutsceneBars"},
    {kDisplayTab, kFpsCounter, "FPS Counter", "The frame rate, at the top right of the screen.", "cg_drawFPS"},
    {kGraphicsTab, kSection, "Preset", "", ""},
    {kGraphicsTab, kPreset, "Graphics Preset", "Original is the game as it shipped. Remastered turns on SMAA, Ambient Occlusion, Depth of Field When Aiming, Blur Gun Sight and Bodies Stay, with Level of Detail at High and Anisotropic Filtering at 16x. Change any of these yourself and it reads Custom.", ""},
    {kGraphicsTab, kSection, "Quality", "", ""},
    {kGraphicsTab, kRenderResolution, "Render Resolution", "The size the game is drawn at, as a share of the window. Over 100 is sharper and costs frame rate, under it is the other way.", "r_renderScale"},
    {kGraphicsTab, kAniso, "Anisotropic Filtering", "Keeps the ground and walls sharp at a shallow angle.", "r_anisotropy"},
    {kGraphicsTab, kMsaa, "MSAA", "Smooths the jagged edges of soldiers, buildings and the ground. Higher is smoother and costs frame rate.", "r_msaa"},
    {kGraphicsTab, kSmaa, "SMAA", "Smooths jagged edges across the whole picture without making it blurry. Can be used with MSAA.", "r_smaa"},
    {kGraphicsTab, kAmbientOcclusion, "Ambient Occlusion", "Soft shade where surfaces meet: in corners, under ledges and around objects. SSAO is the lighter of the two, GTAO the more exact.", "r_ambientOcclusion"},
    {kGraphicsTab, kAdsDof, "Depth of Field When Aiming", "While you aim down the sights, what is in the sight picture stays sharp and everything around it goes out of focus.", "r_dof"},
    {kGraphicsTab, kGunBlur, "Blur Gun Sight", "While you aim down the sights, the part of the gun nearest your eye goes softly out of focus.", "r_blurGunSight"},
    {kGraphicsTab, kViewDistance, "Level of Detail", "How far away soldiers and objects keep their detailed models.", "cg_viewDistance"},
    {kGraphicsTab, kPlayerBody, "Player Body", "Look down and you see your own chest, legs and boots.", "cg_playerBody"},
    {kGraphicsTab, kKeepBodies, "Bodies Stay", "The fallen stay where they fell and are not cleared away.", "cg_bodiesStay"},
    {kGraphicsTab, kSection, "Textures", "", ""},
    {kGraphicsTab, kTextureReplacement, "Texture Replacement", "Pictures in textures\\replacement beside the game take the place of the game's own.", "r_customTextures"},
    {kGraphicsTab, kTextureDumping, "Texture Dumping", "Writes each texture the game uses into textures\\dumps beside the game, once.", "r_dumpTextures"},
    {kGraphicsTab, kDumpFormat, "Dump Format", "The kind of file the dumped textures are written as.", ""},
    {kGameplayTab, kSection, "Play", "", ""},
    {kGameplayTab, kAimAssist, "Controller Aim Assist", "The game's pull towards an enemy under the sight, on a controller.", "cg_aimAssist"},
    {kGameplayTab, kHints, "Tutorial Hints", "The lines that tell you how to crouch, lean, throw and the like.", "cg_hints"},
    {kGameplayTab, kHideHud, "Hide the HUD", "Takes the compass, the health and the ammunition off the screen.", "cg_drawHud"},
    {kGameplayTab, kGCrosshair, "Crosshair", "The crosshair in the middle of the screen when you fire from the hip.", ""},
    {kGameplayTab, kGTags, "Friendly Tags", "The names over the men of your own side.", ""},
    {kGameplayTab, kGSubtitles, "Subtitles", "What is said, written at the foot of the screen.", ""},
    {kGameplayTab, kGTank, "Alternate Tank Control", "The tank's other way of steering, with the turret on the right stick alone.", ""},
    {kGameplayTab, kAimToggle, "Aim Down Sights", "Hold: you aim while the key is down. Toggle: a press brings the gun up and the next one lowers it. For the keyboard and mouse.", "cg_aimToggle"},
    {kGameplayTab, kLeanToggle, "Leaning", "Hold: you lean while the key is down. Toggle: a press leans and the next one straightens you.", "cg_leanToggle"},
    {kGameplayTab, kSection, "Menus", "", ""},
    {kGameplayTab, kQwerty, "Profile Name Keys", "The order of the letters on the screen where a profile is named.", "cg_qwertyNames"},
    {kGameplayTab, kPrompts, "Button Prompts", "Whether the game shows keys or a controller's buttons. Automatic follows what you last used.", "cg_buttonPrompts"},
    {kAudioTab, kSection, "Volume", "", ""},
    {kAudioTab, kGVolume, "Master Volume", "How loud the whole game is.", ""},
    {kAudioTab, kMusicVolume, "Music", "How loud the music is in a mission.", "snd_musicVolume"},
    {kAudioTab, kMenuMusicVolume, "Main Menu Music", "How loud the music is in the menus.", "snd_menuMusicVolume"},
    {kAudioTab, kEffectsVolume, "Sound Effects", "Gunfire, explosions, vehicles and the menus' own sounds.", "snd_effectsVolume"},
    {kAudioTab, kVoiceVolume, "Dialogue", "The voices of your squad and the enemy.", "snd_voiceVolume"},
    {kControlsTab, kSection, "Mouse", "", ""},
    {kControlsTab, kMouseX, "Horizontal Sensitivity", "How far a move of the mouse turns you left and right.", "m_sensitivityX"},
    {kControlsTab, kMouseY, "Vertical Sensitivity", "How far a move of the mouse looks up and down.", "m_sensitivityY"},
    {kControlsTab, kInvertY, "Invert Vertical Look", "Pushing the mouse away looks down.", "m_invertMouse"},
    {kControlsTab, kInvertX, "Invert Horizontal Look", "Moving the mouse left looks right.", "m_invertMouseX"},
    // the Controller tab is the game's Controller screen: these in two columns of three, the picture under them
    {kControllerTab, kGInvert, "Invert Aim", "Pushing the right stick away looks down.", ""},
    {kControllerTab, kGVibration, "Vibration", "The controller shakes with gunfire and explosions.", ""},
    {kControllerTab, kGAdsToggle, "Aim Down Sights", "Hold the button to aim, or press it once to bring the gun up.", ""},
    {kControllerTab, kGSensX, "Horizontal Sensitivity", "How fast the right stick turns you left and right.", ""},
    {kControllerTab, kGSensY, "Vertical Sensitivity", "How fast the right stick looks up and down.", ""},
    {kControllerTab, kGLayout, "Controller Configuration", "Which button does what. A to D are the game's, Modern is this port's.", ""},
};

constexpr struct {
    game::cheats::Toggle toggle;
    const char* label;
    const char* note;
} kCheatRows[] = {
    {game::cheats::kGod, "God Mode", "Nothing hurts you."},
    {game::cheats::kNoTarget, "Enemies Ignore You", "The enemy does not see you."},
    {game::cheats::kLowGravity, "Low Gravity", "You fall slowly. The world keeps its own."},
    {game::cheats::kFastRun, "Run Fast", "Twice your speed."},
    {game::cheats::kBackwards, "sdrawkcaB", "Moving, looking and the HUD are turned round."},
};

// what each line of the menu's page is
struct ModernRow {
    enum Kind { kNone, kSetting, kBind, kResetKeys, kCheat, kAmmo, kHeal, kPassword } kind = kNone;
    RowId row = kSection;
    size_t action = 0;
    int cheat = 0;
    const char* note = "";
    const char* variable = "";
};

struct ModernState {
    int tab = 0;
    int selected = -1;
    float scroll = 0;
    bool follow = true;
    ImVec2 mouse{-1, -1};
    uint32_t pad_prev = 0;
    double pad_repeat = 0;
    bool controller = false;  // the controller was the last thing used: the bottom line shows its buttons
    std::string code;         // the cheats password as typed so far
    int drag = -1;            // the slider line the mouse holds, while its button is down
    bool held = false;        // the left button, as Windows had it last frame
    bool confirm = false;     // leaving with changes waiting: the slip that asks about them is up
    int confirm_choice = 0;
    int editing = -1;         // the line picked with A or Enter: left and right change it until it is let go
    bool pointer = false;     // the mouse was the last thing used: its lines change at a click, without the pick
    ui::modern::Hits hits;
} g_modern;

// A slider row's range and step, and its value in a set of settings; false for a row that is not one. Every one
// steps by one. The mouse's sensitivities are in hundredths.
struct SliderRange {
    float lo, hi, step;
};
bool Slider(RowId id, SliderRange& r) {
    switch (id) {
        case kFov: r = {float(game::kMinFov), float(game::kMaxFov), 1}; return true;
        case kBrightness: r = {50, 200, 1}; return true;
        case kRenderResolution: r = {20, 200, 1}; return true;
        case kMouseX:
        case kMouseY: r = {10, 500, 1}; return true;
        case kMusicVolume:
        case kMenuMusicVolume:
        case kEffectsVolume:
        case kVoiceVolume: r = {0, 100, 1}; return true;
        case kGVolume: r = {0, 100, 1}; return true;
        case kGSensX:
        case kGSensY: r = {0, float(game::kLookSensitivityMax), 1}; return true;
        default: return false;
    }
}
float SliderValue(RowId id, const Settings& s, const game::Options& o) {
    switch (id) {
        case kFov: return float(o.fov);
        case kBrightness: return float(o.brightness);
        case kRenderResolution: return float(s.render_percent);
        case kMouseX: return o.mouse_sensitivity * 100.0f;
        case kMouseY: return o.mouse_vertical_sensitivity * 100.0f;
        case kMusicVolume: return float(o.music_volume);
        case kMenuMusicVolume: return float(o.menu_music_volume);
        case kEffectsVolume: return float(o.effects_volume);
        case kVoiceVolume: return float(o.voice_volume);
        case kGVolume: return float(o.MasterPercent());
        case kGSensX: return float(o.look_sensitivity_x < 0 ? game::kLookSensitivityDefault : o.look_sensitivity_x);
        case kGSensY: return float(o.look_sensitivity_y < 0 ? game::kLookSensitivityDefault : o.look_sensitivity_y);
        default: return 0;
    }
}
// a slider's place, 0 to 1; under 0 for a row that is not one
float RowFill(RowId id, const Settings& s, const game::Options& o) {
    SliderRange r;
    if (!Slider(id, r)) return -1.0f;
    return std::clamp((SliderValue(id, s, o) - r.lo) / (r.hi - r.lo), 0.0f, 1.0f);
}
// a slider dragged to a place, 0 to 1: the nearest of its steps, held like any other change
void DragSlider(RowId id, float place) {
    SliderRange r;
    if (!Slider(id, r)) return;
    float value = r.lo + std::round(std::clamp(place, 0.0f, 1.0f) * (r.hi - r.lo) / r.step) * r.step;
    HoldPanel();
    Settings& s = g_panel_settings;
    game::Options& o = g_panel_options;
    uint32_t whole = uint32_t(value + 0.5f);
    switch (id) {
        case kFov: o.fov = whole; break;
        case kBrightness: o.brightness = whole; break;
        case kRenderResolution: s.render_percent = whole; break;
        case kMouseX: o.mouse_sensitivity = float(whole) / 100.0f; break;
        case kMouseY: o.mouse_vertical_sensitivity = float(whole) / 100.0f; break;
        case kMusicVolume: o.music_volume = whole; break;
        case kMenuMusicVolume: o.menu_music_volume = whole; break;
        case kEffectsVolume: o.effects_volume = whole; break;
        case kVoiceVolume: o.voice_volume = whole; break;
        case kGVolume:
            o.master_volume = int32_t(whole);
            o.game_options[game::kGoVolume] = (int32_t(whole) + 1) / 2;
            break;
        case kGSensX: o.look_sensitivity_x = int32_t(whole); break;
        case kGSensY: o.look_sensitivity_y = int32_t(whole); break;
        default: break;
    }
}

// a row's words with what is in use, whatever is held
std::string LiveText(RowId id) {
    game::Options* held = g_row_options;
    g_row_options = nullptr;
    std::string text = RowText(id, 0, Get());
    g_row_options = held;
    return text;
}

// what each button does in a controller configuration (the game's style_h/e/f/g.cfg for A to D, pad_layout.cpp's
// binds for Modern), in the order of ui::modern::PadLabel: RT, RB, X, Y, B, A, LT, LB, the left stick, its click, the
// pad's up, left and down, its right, Back, Start, the right stick's click, the right stick. "ADS" is the aim button
constexpr const char* kPadWords[5][ui::modern::kPadLabels] = {
    {"Attack", "Activate", "Grenade", "Up/Jump", "Lower Stance", "Melee", "ADS", "Reload", "Move", "Sprint",
     "Next Weapon", "Lean Left", "Next Weapon", "Lean Right", "Objectives", "Pause", "N/A", "Look"},
    {"Attack", "Raise Stance", "Activate/Reload", "Next Weapon", "Melee", "Up/Jump", "Grenade", "Lower Stance", "Move",
     "Sprint", "Raise Stance", "Lean Left", "Lower Stance", "Lean Right", "Objectives", "Pause", "ADS", "Look"},
    {"Attack", "Activate", "Reload", "Up/Jump", "Lower Stance", "Melee", "Grenade", "Next Weapon", "Move", "Sprint",
     "Next Weapon", "Lean Left", "Prev Weapon", "Lean Right", "Objectives", "Pause", "ADS", "Look"},
    {"Attack", "Reload", "Melee", "Activate", "Sprint", "Up/Jump", "Grenade", "Next Weapon", "Move", "Lower Stance",
     "Raise Stance", "Lean Left", "Lower Stance", "Lean Right", "Objectives", "Pause", "ADS", "Look"},
    {"Attack", "Grenade", "Activate/Reload", "Next Weapon", "Crouch/Prone", "Up/Jump", "ADS", "Grenade", "Move",
     "Sprint", "Objectives", "Lean Left", "Prev Weapon", "Lean Right", "Objectives", "Pause", "Melee", "Look"},
};

// the Controller tab's picture for the configuration picked on the page (waiting for Apply or not)
void BuildPad(ui::modern::Page& page) {
    std::string picked = PanelText(kGLayout, 0);
    int layout = 0;
    for (int i = 0; i < 5; i++)
        if (picked == kLayoutNames[i]) layout = i;
    bool toggle = PanelText(kGAdsToggle, 0) == "TOGGLE";
    page.pad = true;
    page.pad_modern = layout == 4;
    for (int i = 0; i < ui::modern::kPadLabels; i++) {
        std::string word = kPadWords[layout][i];
        if (word == "ADS" && toggle) word = "Toggle ADS";
        if (i == ui::modern::kPadL3 || i == ui::modern::kPadR3) word = "(Click) " + word;
        page.pad_labels[i] = word;
    }
}

// the lines waiting for Apply, over every tab
int ModernChanges() {
    int n = 0;
    if (g_panel_pending)
        for (const TabRow& r : kTabRows)
            if (r.id != kSection && r.id != kPreset && PanelText(r.id, 0) != LiveText(r.id)) n++;  // (the preset is the settings it stands for)
    const HeldCheats& h = g_held_cheats;
    if (h.pending) {
        for (int t = 0; t < game::cheats::kToggles; t++)
            if (h.on[t] != game::cheats::On(game::cheats::Toggle(t))) n++;
        if (h.ammo != game::cheats::UnlimitedAmmo()) n++;
        if (h.heal) n++;
    }
    return n;
}

void BuildModern(ui::modern::Page& page, std::vector<ModernRow>& rows) {
    using ui::modern::Row;
    namespace cheats = game::cheats;
    page.rows.clear();
    rows.clear();
    auto add = [&](Row r, ModernRow m) {
        page.rows.push_back(std::move(r));
        rows.push_back(m);
    };
    auto heading = [&](const char* label) {
        Row r;
        r.kind = Row::kHeading;
        r.label = label;
        add(r, {});
    };
    auto words = [&](const char* text) {
        Row r;
        r.kind = Row::kText;
        r.label = text;
        add(r, {});
    };
    int tab = g_modern.tab;
    for (const TabRow& t : kTabRows) {
        if (t.tab != tab) continue;
        if (t.id == kSection) {
            heading(t.label);
            continue;
        }
        Row r;
        r.label = t.label;
        std::string text = PanelText(t.id, 0);
        r.changed = g_panel_pending && text != LiveText(t.id);
        float fill = RowFill(t.id, g_panel_pending ? g_panel_settings : Get(), g_panel_pending ? g_panel_options : game::Opt());
        if (fill >= 0) {
            r.kind = Row::kSlider;
            r.fill = fill;
            r.value = text.substr(0, text.find(" ("));
        } else if (t.id != kMsaa && t.id != kAmbientOcclusion && (text == "ON" || text == "OFF")) {  // MSAA and Ambient Occlusion have Off among their choices
            r.kind = Row::kToggle;
            r.on = text == "ON";
        } else {
            r.kind = Row::kChoice;
            r.value = Sentence(text);
            r.fixed = t.id == kResolution &&
                      (g_panel_pending ? g_panel_settings : Get()).display_mode == DisplayMode::Borderless;
        }
        ModernRow m;
        m.kind = ModernRow::kSetting;
        m.row = t.id;
        m.note = t.note;
        m.variable = t.variable;
        add(r, m);
    }
    if (tab == kControlsTab) {
        heading("Key Bindings");
        size_t count = 0;
        input::Actions(&count);
        for (size_t i = 0; i < count; i++) {
            Row r;
            r.kind = Row::kBinding;
            r.label = TitleCase(input::Label(i));
            r.value = input::Binding(i, 0);
            r.value2 = input::Binding(i, 1);
            ModernRow m;
            m.kind = ModernRow::kBind;
            m.action = i;
            m.note = "Click a box or press Enter, then press the new key. Delete or a right click empties a box. The keys do "
                     "the same whichever controller layout is picked in the game's options.";
            add(r, m);
        }
        Row r;
        r.kind = Row::kAction;
        r.label = "Reset Controls";
        ModernRow m;
        m.kind = ModernRow::kResetKeys;
        m.note = "Every key back to the one it came with, at once.";
        add(r, m);
    }
    if (tab == kCheatsTab) {
        const char* risk = "Cheats are on until the game is closed. They can break missions: use them at your own risk.";
        if (!cheats::Unlocked()) {
            words("Cheats are locked. Type the password.");
            Row r;
            r.kind = Row::kEntry;
            r.label = "Password";
            r.value = g_modern.code;
            ModernRow m;
            m.kind = ModernRow::kPassword;
            m.note = risk;
            add(r, m);
            return;
        }
        const HeldCheats& held = g_held_cheats;
        heading("Cheats");
        for (int i = 0; i < int(std::size(kCheatRows)); i++) {
            Row r;
            r.kind = Row::kToggle;
            r.label = kCheatRows[i].label;
            bool live = cheats::On(kCheatRows[i].toggle);
            r.on = held.pending ? held.on[kCheatRows[i].toggle] : live;
            r.changed = r.on != live;
            ModernRow m;
            m.kind = ModernRow::kCheat;
            m.cheat = i;
            m.note = kCheatRows[i].note;
            add(r, m);
        }
        Row ammo;
        ammo.kind = Row::kToggle;
        ammo.label = "Unlimited Ammo";
        ammo.on = held.pending ? held.ammo : cheats::UnlimitedAmmo();
        ammo.changed = ammo.on != cheats::UnlimitedAmmo();
        ModernRow m;
        m.kind = ModernRow::kAmmo;
        m.note = "Your rounds never go down.";
        add(ammo, m);
        Row heal;
        heal.kind = Row::kToggle;
        heal.label = "Full Health";
        heal.on = held.pending && held.heal;
        heal.changed = heal.on;
        m.kind = ModernRow::kHeal;
        m.note = "Your health back to full, once, when you pick Apply Changes.";
        add(heal, m);
        words(risk);
    }
}

bool ModernStop(const ui::modern::Row& r) {
    return r.kind != ui::modern::Row::kHeading && r.kind != ui::modern::Row::kText;
}

int ModernNext(const ui::modern::Page& page, int from, int dir) {
    int n = int(page.rows.size());
    for (int k = 1; k <= n; k++) {
        int i = ((from + dir * k) % n + n) % n;
        if (ModernStop(page.rows[size_t(i)])) return i;
    }
    return from;
}

// a line's job. step: -1 or 1 from the arrows, 0 from Enter or A; want: for an On and Off line, 1 On, 0 Off, -1 the
// other one
void ModernActivate(const ModernRow& r, const ui::modern::Row& shown, int step, int want = -1) {
    namespace cheats = game::cheats;
    bool flip = want < 0 || (want == 1) != shown.on;
    switch (r.kind) {
        case ModernRow::kSetting:
            if (shown.kind == ui::modern::Row::kToggle) {
                if (flip) PanelText(r.row, 1);
            } else if (shown.kind == ui::modern::Row::kSlider) {
                // a slider stops at its ends
                if (step == 0 || (step < 0 && shown.fill <= 0.0f) || (step > 0 && shown.fill >= 1.0f)) break;
                PanelText(r.row, step);
            } else {
                PanelText(r.row, step ? step : 1);
            }
            break;
        case ModernRow::kBind:
            if (step == 0) g_capture = {int(r.action), g_slot, false};
            break;
        case ModernRow::kResetKeys:
            if (step == 0) {
                g_capture = {};
                input::ResetBindings();
            }
            break;
        case ModernRow::kCheat:
            if (flip) {
                HoldCheats();
                bool& on = g_held_cheats.on[kCheatRows[r.cheat].toggle];
                on = !on;
            }
            break;
        case ModernRow::kAmmo:
            if (flip) {
                HoldCheats();
                g_held_cheats.ammo = !g_held_cheats.ammo;
            }
            break;
        case ModernRow::kHeal:
            if (flip) {
                HoldCheats();
                g_held_cheats.heal = !g_held_cheats.heal;
            }
            break;
        default:
            break;
    }
}

// F10 or Escape over the settings menu (the window's messages): it leaves as Backspace does, asking first
std::atomic<bool> g_close_request{false};

void DrawModern() {
    ModernState& p = g_modern;
    ui::modern::Page page;
    std::vector<ModernRow> rows;
    auto build = [&] {
        BuildModern(page, rows);
        if (p.selected < 0 || p.selected >= int(page.rows.size()) || !ModernStop(page.rows[size_t(p.selected)]))
            p.selected = ModernNext(page, -1, 1), p.follow = true;
    };
    build();

    bool was_capturing = g_capture.action >= 0;
    UpdateCapture();
    bool capturing = g_capture.action >= 0;

    // the controller: edges, and the dpad repeating while held
    uint32_t pad = g_paper_pad.load(std::memory_order_relaxed);
    uint32_t pressed = pad & ~p.pad_prev;
    double now = ImGui::GetTime();
    constexpr uint32_t kDirs = kPadUp | kPadDown | kPadLeft | kPadRight;
    if (pressed & kDirs) p.pad_repeat = now + 0.4;
    else if ((pad & kDirs) && now >= p.pad_repeat) {
        pressed |= pad & kDirs;
        // a slider held left or right runs: its steps are single ones
        bool slider = p.selected >= 0 && p.selected < int(page.rows.size()) &&
                      page.rows[size_t(p.selected)].kind == ui::modern::Row::kSlider && (pad & (kPadLeft | kPadRight));
        p.pad_repeat = now + (slider ? 0.03 : 0.12);
    }
    p.pad_prev = pad;
    if (pressed) p.controller = true;

    ImGuiIO& io = ImGui::GetIO();
    // the console over the menu has the keys, the mouse and the controller until it is put away
    bool console_up = console::Open();
    if (console_up) pressed = 0;
    // leaving: at once with nothing waiting, else the slip asks (Apply, Discard, Cancel)
    bool close = g_close_request.exchange(false, std::memory_order_relaxed);
    if (p.editing != p.selected) p.editing = -1;
    if (close && p.editing >= 0 && !console_up) {  // Escape lets go of the line first
        p.editing = -1;
        close = false;
    }
    auto leave = [&] {
        if (ModernChanges() > 0) {
            g_capture = {};
            p.confirm = true;
            p.confirm_choice = 0;
            p.drag = -1;
            p.editing = -1;
            return false;
        }
        ToggleMenu();
        return true;
    };
    constexpr const char* kConfirmChoices[] = {"Apply", "Discard", "Cancel"};
    if (p.confirm && console_up) {
        if (close) p.confirm = false;
    } else if (p.confirm) {
        auto once = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
        int n = int(std::size(kConfirmChoices));
        if (once(ImGuiKey_LeftArrow) || once(ImGuiKey_UpArrow) || (pressed & (kPadLeft | kPadUp)))
            p.confirm_choice = (p.confirm_choice + n - 1) % n;
        if (once(ImGuiKey_RightArrow) || once(ImGuiKey_DownArrow) || once(ImGuiKey_Tab) || (pressed & (kPadRight | kPadDown)))
            p.confirm_choice = (p.confirm_choice + 1) % n;
        int pick = -1;
        if (once(ImGuiKey_Enter) || once(ImGuiKey_KeypadEnter) || (pressed & kPadA)) pick = p.confirm_choice;
        if (close || once(ImGuiKey_Backspace) || (pressed & kPadB)) pick = 2;
        ImVec2 m = io.MousePos;
        bool moved = m.x != p.mouse.x || m.y != p.mouse.y;
        p.mouse = m;
        bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (moved || click) p.controller = false;
        for (size_t i = 0; i < p.hits.confirm.size() && i < size_t(n); i++)
            if (p.hits.confirm[i].Has(m)) {
                if (moved || click) p.confirm_choice = int(i);
                if (click) pick = int(i);
            }
        p.held = true;  // a click on the slip is not one on the page under it
        if (pick == 0) ApplyPanel();
        if (pick == 0 || pick == 1) {
            ToggleMenu();
            return;
        }
        if (pick == 2) p.confirm = false;
    } else if (close && leave()) {
        return;
    }
    bool typing = rows[size_t(p.selected)].kind == ModernRow::kPassword;
    auto to_tab = [&](int tab) {
        p.tab = ((tab % kTabs) + kTabs) % kTabs;
        p.selected = -1;
        p.scroll = 0;
        p.drag = -1;
        p.editing = -1;
        build();
    };
    // capture takes every key until it ends; a key that ended it does nothing else this frame
    if (!capturing && !was_capturing && !p.confirm && !console_up) {
        auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        auto once = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
        int move = 0, step = 0, turn = 0;
        if (key(ImGuiKey_UpArrow) || (pressed & kPadUp)) move = -1;
        if (key(ImGuiKey_DownArrow) || (pressed & kPadDown)) move = 1;
        if (key(ImGuiKey_LeftArrow) || (pressed & kPadLeft)) step = -1;
        if (key(ImGuiKey_RightArrow) || (pressed & kPadRight)) step = 1;
        // the letters are the password's while it is being typed
        if (once(ImGuiKey_PageUp) || (!typing && once(ImGuiKey_Q)) || (pressed & kPadPrev)) turn = -1;
        if (once(ImGuiKey_PageDown) || once(ImGuiKey_Tab) || (!typing && once(ImGuiKey_E)) || (pressed & kPadNext)) turn = 1;
        bool enter = once(ImGuiKey_Enter) || once(ImGuiKey_KeypadEnter) || (pressed & kPadA);
        bool back = (!typing && once(ImGuiKey_Backspace)) || (pressed & kPadB);
        bool clear = once(ImGuiKey_Delete) || (pressed & kPadX);
        bool defaults = (!typing && once(ImGuiKey_R)) || ((pressed & kPadX) && rows[size_t(p.selected)].kind != ModernRow::kBind);
        bool apply = (!typing && once(ImGuiKey_F)) || (pressed & kPadY);
        if (typing) {
            for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
                ImWchar ch = io.InputQueueCharacters[i];
                if (ch > 32 && ch < 127 && p.code.size() < 15) p.code += char(ch);
            }
            if (key(ImGuiKey_Backspace) && !p.code.empty()) p.code.pop_back();
            if (game::cheats::TryUnlock(p.code.c_str())) {
                p.code.clear();
                p.selected = -1;
                build();
            }
        }
        // a setting is picked with A or Enter before left and right change it, and let go with A, Enter or back
        {
            ui::modern::Row::Kind shown = page.rows[size_t(p.selected)].kind;
            bool editable = rows[size_t(p.selected)].kind != ModernRow::kBind &&
                            (shown == ui::modern::Row::kChoice || shown == ui::modern::Row::kSlider ||
                             shown == ui::modern::Row::kToggle);
            if (p.editing >= 0) {
                if (enter || back) p.editing = -1;
                enter = back = clear = defaults = false;
                move = turn = 0;
            } else if (editable) {
                if (enter) p.editing = p.selected;
                if (enter || step) p.pointer = false;
                enter = false;
                step = 0;
            }
        }
        if (back) {
            if (leave()) return;
            turn = move = step = 0;
            enter = clear = defaults = apply = false;
        }
        if (turn) to_tab(p.tab + turn);
        if (move) {
            p.selected = ModernNext(page, p.selected, move);
            p.follow = true;
        }
        if (step) {
            if (rows[size_t(p.selected)].kind == ModernRow::kBind) g_slot = step < 0 ? 0 : 1;
            else if (page.rows[size_t(p.selected)].kind == ui::modern::Row::kToggle)  // On is the left box
                ModernActivate(rows[size_t(p.selected)], page.rows[size_t(p.selected)], 0, step < 0 ? 1 : 0);
            else ModernActivate(rows[size_t(p.selected)], page.rows[size_t(p.selected)], step);
        }
        if (enter) ModernActivate(rows[size_t(p.selected)], page.rows[size_t(p.selected)], 0);
        if (clear && rows[size_t(p.selected)].kind == ModernRow::kBind) input::Bind(rows[size_t(p.selected)].action, 0, g_slot);
        if (defaults) Defaults();
        if (apply) ApplyPanel();
        if (!pressed && (move || step || turn || enter || io.InputQueueCharacters.Size)) p.controller = false;
        if (move || step || turn || enter) p.pointer = false;

        // the mouse: hovering selects, a click does the line's job, the wheel scrolls
        // the pointer and its left button are read from Windows each frame, as the game's own pointer is drawn: a
        // slider held and slid is followed whatever becomes of the window's mouse messages
        ImVec2 m = io.MousePos;
        bool held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        {
            HWND hwnd = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
            POINT point;
            RECT client;
            if (hwnd && GetForegroundWindow() == hwnd && GetCursorPos(&point) && ScreenToClient(hwnd, &point) &&
                GetClientRect(hwnd, &client) && client.right > 0 && client.bottom > 0) {
                m = ImVec2(float(point.x) * io.DisplaySize.x / float(client.right),
                           float(point.y) * io.DisplaySize.y / float(client.bottom));
                held = GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0;
            }
        }
        bool moved = m.x != p.mouse.x || m.y != p.mouse.y;
        p.mouse = m;
        bool left = (held && !p.held) || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (left && p.held) left = false;  // one press, once
        bool right = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (moved || left || right) p.controller = false, p.pointer = true, p.editing = -1;
        const ui::modern::Hits& h = p.hits;
        if (left) {
            int tab = p.tab;
            bool picked = false;
            for (size_t i = 0; i < h.tabs.size(); i++)
                if (h.tabs[i].Has(m)) tab = int(i), picked = true;
            if (h.tab_prev.Has(m)) tab = p.tab - 1, picked = true;
            if (h.tab_next.Has(m)) tab = p.tab + 1, picked = true;
            if (picked) to_tab(tab);
            else if (h.defaults.Has(m)) Defaults();
            else if (h.apply.Has(m)) ApplyPanel();
        }
        int over = -1;
        for (size_t i = 0; i < h.rows.size() && i < rows.size(); i++)
            if (h.rows[i].row.Has(m)) over = int(i);
        if (over >= 0 && (moved || left || right)) {
            const ui::modern::Hits::Line& l = h.rows[size_t(over)];
            const ModernRow& o = rows[size_t(over)];
            const ui::modern::Row& shown = page.rows[size_t(over)];
            p.selected = over;
            bool in_left = l.left.Has(m), in_right = l.right.Has(m);
            if (o.kind == ModernRow::kBind) {
                if (in_left) g_slot = 0;
                if (in_right) g_slot = 1;
                if (left && (in_left || in_right)) ModernActivate(o, shown, 0);
                if (right && (in_left || in_right)) input::Bind(o.action, 0, g_slot);
            } else if (shown.kind == ui::modern::Row::kToggle) {
                if (left && (in_left || in_right)) ModernActivate(o, shown, 0, in_left ? 1 : 0);
            } else if (shown.kind == ui::modern::Row::kAction) {
                if (left) ModernActivate(o, shown, 0);
            } else if (shown.kind != ui::modern::Row::kSlider && left && (in_left || in_right)) {
                ModernActivate(o, shown, in_left ? -1 : 1);
            }
        }
        // a slider's bar is held and slid: the handle follows the pointer until the button is let go
        if (left && over >= 0 && h.rows[size_t(over)].bar.Has(m) && rows[size_t(over)].kind == ModernRow::kSetting) p.drag = over;
        if (!held) p.drag = -1;
        p.held = held;
        if (p.drag >= 0 && p.drag < int(rows.size()) && size_t(p.drag) < h.rows.size()) {
            const ui::modern::Rect& bar = h.rows[size_t(p.drag)].bar;
            if (bar.max.x > bar.min.x) {
                p.selected = p.drag;
                DragSlider(rows[size_t(p.drag)].row, (m.x - bar.min.x) / (bar.max.x - bar.min.x));
            }
        }
        if (io.MouseWheel != 0) {
            p.scroll = std::max(0.0f, p.scroll - io.MouseWheel * 2.0f);
            p.follow = false;
        }
        build();  // the values as they are now
    }

    ui::modern::Row& sel = page.rows[size_t(p.selected)];
    const ModernRow& row = rows[size_t(p.selected)];
    sel.slot = g_slot;
    sel.waiting = capturing;
    sel.editing = p.editing == p.selected;
    sel.locked = !sel.editing && !p.pointer;
    page.title = "Options";
    const std::string& profile = game::Opt().settings_profile;
    if (!profile.empty() && profile != "*") page.profile = profile;
    page.tabs.assign(std::begin(kTabNames), std::end(kTabNames));
    page.tab = p.tab;
    page.selected = p.selected;
    page.follow = p.follow;
    page.controller = p.controller;
    page.changes = ModernChanges();
    page.note_title = row.kind == ModernRow::kBind ? "Key Bindings" : sel.label;
    page.note_text = row.note;
    page.note_command = row.variable;
    if (row.kind == ModernRow::kSetting && sel.changed)
        page.note_change = "Now " + Sentence(LiveText(row.row)) + ", after Apply " + Sentence(PanelText(row.row, 0));
    if (p.tab == kControllerTab) {
        BuildPad(page);
        page.hint = row.note;  // no slip on this tab: its words go over the foot
    }
    if (capturing) page.hint = "Press a key for it. Escape keeps the old one.";
    else if (sel.editing)
        page.hint = p.controller ? "Left and right change it. Press A when done." : "Left and right change it. Press Enter when done.";
    page.confirm = p.confirm;
    page.confirm_choices.assign(std::begin(kConfirmChoices), std::end(kConfirmChoices));
    page.confirm_selected = p.confirm_choice;
    ui::modern::Draw(page, p.scroll, p.hits);
    p.follow = false;
}

// top right, over the game, in Hack as the console is
void DrawFpsCounter(double fps) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImFont* font = ::ui::paper::Mono();
    float em = font ? ::ui::paper::MonoSize() : ImGui::GetFontSize();
    if (!font) font = ImGui::GetFont();
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    ImVec2 size = font->CalcTextSizeA(em, FLT_MAX, 0.0f, text);
    ImVec2 pos(std::floor(vp->Pos.x + vp->Size.x - size.x - em), std::floor(vp->Pos.y + em * 0.6f));
    float shadow = std::max(1.0f, std::round(em / 16.0f));
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddText(font, em, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 200), text);
    list->AddText(font, em, pos, ImGui::GetColorU32(kHeading), text);
}

// a message from the cheats (src/game/cheats.cpp), such as the thereisacow1337 profile's: a line at the top of the screen for two seconds, then fading
void DrawProfileMessage() {
    constexpr double kIn = 0.3, kHeld = 2.0, kOut = 0.8, kShown = kHeld + kOut;
    double age = 0;
    const char* message = game::cheats::Message(age);
    if (!message || age < 0 || age > kShown) return;
    float alpha = float(std::min({age / kIn, (kShown - age) / kOut, 1.0}));
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImFont* font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 1.5f, em = size;
    ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, message);
    ImVec2 pos(vp->Pos.x + (vp->Size.x - text.x) * 0.5f, vp->Pos.y + vp->Size.y * 0.06f);
    ImVec2 a(pos.x - em * 0.9f, pos.y - em * 0.45f), b(pos.x + text.x + em * 0.9f, pos.y + text.y + em * 0.45f);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(a, b, IM_COL32(12, 10, 6, int(200 * alpha)), em * 0.3f);
    list->AddRect(a, b, IM_COL32(214, 178, 96, int(230 * alpha)), em * 0.3f, 0, std::max(1.0f, em * 0.06f));
    list->AddText(font, size, pos, IM_COL32(240, 214, 140, int(255 * alpha)), message);
}

}  // namespace


namespace pc {

namespace {
// PC Options on the game's screens is a first page of links and a page each for the display, the graphics and the
// controls (the mouse, then the key bindings)
// Leaving a page with changes not applied asks first, on a page of its own: keep editing, discard or apply.
// kGameplayPage is not one of PC Options' own: it is the game's Gameplay screen's second page (OpenExtra)
enum PageId { kMainPage, kDisplayPage, kGraphicsPage, kMousePage, kKeysPage, kGameplayPage, kConfirmPage };
struct PageSpec {
    const char* title;
    int part;  // what Reset to Defaults puts back
    std::vector<RowId> rows;
    bool bindings;
};
const PageSpec& Spec(int page) {
    static const PageSpec specs[] = {
        {"PC Options", kEveryPart, {}, false},
        {"Display", kDisplayPart, {kDisplayMode, kResolution, kRenderResolution, kBrightness, kFov, kMaxFps, kVsync,
          kFpsCounter, kCutsceneBars},
         false},
        {"Graphics", kGraphicsPart, {kAniso, kViewDistance, kKeepBodies, kTextureReplacement, kTextureDumping, kDumpFormat}, false},
        {"Mouse", kMousePart, {kMouseX, kMouseY, kInvertY, kInvertX}, false},
        {"Keyboard", kKeysPart, {kPrompts, kQwerty}, true},
        {"Gameplay", kGameplayPart, {kAimAssist, kHints, kHideHud, kLeanToggle}, false},
    };
    return specs[page];
}
int g_pc_page = kMainPage;
int g_pc_from = kDisplayPage;  // the page the confirm page asks about
bool g_pc_extra = false;       // up as the Gameplay screen's second page: leaving it gives the screen back
bool g_pc_done = false;        // and it has been left
// a page's changes, held until its Apply; Back drops them
Settings g_pc_settings;
game::Options g_pc_options;
bool g_pc_pending = false;

void HoldPage() {
    if (g_pc_pending) return;
    g_pc_settings = Get();
    g_pc_options = game::Opt();
    g_pc_pending = true;
}

// a row's words on the page, stepped first when step is not zero: the held values once anything is held
std::string PageText(RowId id, int step) {
    if (step) HoldPage();
    g_row_options = g_pc_pending ? &g_pc_options : nullptr;
    std::string text = RowText(id, step, g_pc_pending ? g_pc_settings : Get());
    g_row_options = nullptr;
    return text;
}

// something held differs from what is in use
bool PageChanged() {
    return g_pc_pending && RowsDiffer(g_pc_settings, g_pc_options, Get(), game::Opt());
}

void ApplyPage() {
    if (!g_pc_pending) return;
    g_pc_pending = false;
    Settings& s = Get();
    bool display = s.display_mode != g_pc_settings.display_mode || s.width != g_pc_settings.width ||
                   s.height != g_pc_settings.height;
    CopyRows(s, game::Opt(), g_pc_settings, g_pc_options);
    if (display) ApplyDisplay();
    Save();
}
uint32_t g_pc_serial = 0;  // moves with the page
int g_pc_focus = 0;        // the line the selection goes to on the page just opened

struct PcRow {
    enum Kind { kSetting, kBind, kLink, kReset, kApplyExit, kApply, kKeep, kDiscard, kNote } kind;
    RowId row = kSection;
    size_t action = 0;
    int page = kMainPage;
};

void OpenPage(int page, int focus) {
    g_pc_page = page;
    g_pc_focus = focus;
    g_pc_serial++;
    g_capture = {};
    g_slot = 0;
}

const char* Label(RowId id) {
    for (const Row& r : kRows)
        if (r.id == id) return r.label;
    return "";
}

// the page's lines, its two actions last
void Page(std::vector<Line>& lines, std::vector<PcRow>& rows, int selected) {
    lines.clear();
    rows.clear();
    if (g_pc_page == kConfirmPage) {
        Line note, keep, discard, apply;
        note.label = "Changes are not applied yet.";
        note.note = true;
        keep.label = "Keep Editing";
        keep.link = true;
        discard.label = "Discard Changes";
        apply.label = "Apply Changes";
        discard.action = apply.action = true;
        lines = {note, keep, discard, apply};
        rows = {{PcRow::kKeep}, {PcRow::kKeep}, {PcRow::kDiscard}, {PcRow::kApply}};
        return;
    }
    const PageSpec& spec = Spec(g_pc_page);
    if (g_pc_page == kMainPage) {
        for (int page = kDisplayPage; page <= kKeysPage; page++) {
            Line l;
            l.label = Spec(page).title;
            l.link = true;
            lines.push_back(l);
            rows.push_back({PcRow::kLink, kSection, 0, page});
        }
        Line note;
        note.label = kPanelNote;
        note.note = true;
        lines.push_back(note);
        rows.push_back({PcRow::kNote});
    }
    for (RowId id : spec.rows) {
        Line l;
        l.label = Sentence(Label(id));
        l.value = Sentence(PageText(id, 0));
        // beside the long sensitivity labels there is room for the number only
        if (size_t at = l.value.find(" (Default)"); (id == kMouseX || id == kMouseY) && at != std::string::npos)
            l.value.erase(at);
        lines.push_back(l);
        rows.push_back({PcRow::kSetting, id});
    }
    size_t count = 0;
    if (spec.bindings) input::Actions(&count);
    for (size_t i = 0; i < count; i++) {
        Line l;
        l.label = Sentence(input::Label(i));
        l.binding = true;
        l.keys[0] = input::Binding(i, 0);
        l.keys[1] = input::Binding(i, 1);
        for (std::string& key : l.keys)
            if (key.empty()) key = "-";
        // the key box waiting for a key is the one it was picked on, wherever the selection goes
        l.waiting = g_capture.action == int(i);
        l.slot = l.waiting ? (g_capture.slot & 1) : int(lines.size()) == selected ? g_slot : -1;
        if (l.waiting) l.keys[l.slot] = "Press a key";
        l.value = l.keys[0] + "   " + l.keys[1];
        lines.push_back(l);
        rows.push_back({PcRow::kBind, kSection, i});
    }
    Line reset, last;
    reset.label = "Reset to Defaults";
    reset.action = true;
    lines.push_back(reset);
    rows.push_back({PcRow::kReset});
    // the Gameplay screen's second page: its Apply applies the screen's first page too and leaves (menu_mouse.cpp)
    last.label = g_pc_page == kMainPage || g_pc_extra ? "Apply and Exit" : "Apply";
    last.action = true;
    lines.push_back(last);
    rows.push_back({g_pc_page == kMainPage ? PcRow::kApplyExit : PcRow::kApply});
}

// the row of a line, false for none or while a key box waits
bool RowAt(int line, PcRow& row) {
    std::vector<Line> lines;
    std::vector<PcRow> rows;
    Page(lines, rows, -1);
    if (line < 0 || line >= int(rows.size()) || g_capture.action >= 0) return false;
    row = rows[size_t(line)];
    return true;
}
}  // namespace

std::vector<Line> Lines(int selected) {
    std::vector<Line> lines;
    std::vector<PcRow> rows;
    Page(lines, rows, selected);
    return lines;
}

std::string Title() { return g_pc_page == kConfirmPage ? "Unsaved Changes" : Spec(g_pc_page).title; }
uint32_t PageSerial() { return g_pc_serial; }
int Focus() { return g_pc_focus; }

bool Subpage() { return g_pc_page != kMainPage; }

// to the first page, the selection on the link that led to the page left
void ToMainPage() {
    if (g_pc_extra) g_pc_done = true;  // the Gameplay screen's own lines again
    else OpenPage(kMainPage, (g_pc_page == kConfirmPage ? g_pc_from : g_pc_page) - kDisplayPage);
}

void OpenExtra() {
    g_pc_extra = true;
    g_pc_done = false;
    g_pc_pending = false;
    OpenPage(kGameplayPage, 0);
}
bool ExtraDone() { return g_pc_extra && g_pc_done; }

bool Back() {
    if (g_pc_page == kMainPage) return false;
    if (g_pc_page == kConfirmPage) {  // asked and backed out of: still editing
        OpenPage(g_pc_from, 0);
    } else if (PageChanged()) {  // changes not applied: ask first
        g_pc_from = g_pc_page;
        OpenPage(kConfirmPage, 1);
    } else {
        g_pc_pending = false;
        ToMainPage();
    }
    return true;
}

void Step(int line, int dir) {
    PcRow r;
    if (!RowAt(line, r)) return;
    if (r.kind == PcRow::kBind) g_slot = dir < 0 ? 0 : 1;
    else if (r.kind == PcRow::kSetting) PageText(r.row, dir);
}

void Press(int line) {
    PcRow r;
    if (!RowAt(line, r)) return;
    switch (r.kind) {
        case PcRow::kSetting: PageText(r.row, 1); break;
        case PcRow::kBind: g_capture = {int(r.action), g_slot, false}; break;
        case PcRow::kLink: OpenPage(r.page, 0); break;
        case PcRow::kReset:
            // the page's settings as they come, held like any change; the keys go back at once, as a key binds at once
            HoldPage();
            SetDefaults(g_pc_settings, g_pc_options, Spec(g_pc_page).part);
            if (Spec(g_pc_page).part & kKeysPart) input::ResetBindings();
            break;
        case PcRow::kApplyExit:
            ApplyPage();
            break;
        case PcRow::kApply:
            ApplyPage();
            ToMainPage();
            break;
        case PcRow::kNote: break;
        case PcRow::kKeep: OpenPage(g_pc_from, 0); break;
        case PcRow::kDiscard:
            g_pc_pending = false;
            ToMainPage();
            break;
    }
}

void Clear(int line) {
    PcRow r;
    if (RowAt(line, r) && r.kind == PcRow::kBind) input::Bind(r.action, 0, g_slot);
}

void Update() { UpdateCapture(); }

bool Capturing() { return g_capture.action >= 0; }

void Leave(bool applied) {
    if (applied) ApplyPage();
    g_pc_pending = false;
    g_held_set = false;
    g_capture = {};
    g_pc_extra = g_pc_done = false;
    g_pc_page = kMainPage;
    g_pc_focus = 0;
    g_pc_serial++;
    Save();
}

}  // namespace pc

// the panel, the PC Options page or the console (console.h): any of them has the keys, the mouse and the controller
bool MenuOpen() { return g_open.load(std::memory_order_relaxed) || console::Open(); }
bool PanelOpen() { return g_open.load(std::memory_order_relaxed); }
// the settings menu is up and the controller was the last thing used on it (the frame's thread)
bool PanelOnController() { return PanelOpen() && g_modern.controller && !console::Open(); }

bool Capturing() { return g_capture.action >= 0; }

void ToggleMenu() {
    bool open = !PanelOpen();
    if (open && console::Open()) console::Toggle();
    g_open.store(open, std::memory_order_relaxed);
    if (!open) {
        g_bindings_only.store(false, std::memory_order_relaxed);
        g_paper.store(false, std::memory_order_relaxed);
    }
    g_held_set = false;
    g_panel_pending = false;
    g_held_cheats = {};
    g_capture = {};
    if (open) {
        g_modern = {};
        g_close_request.store(false, std::memory_order_relaxed);
        g_modern.pad_prev = ~0u;  // a button held as it opens (the A that picked PC Options) is let go of first
        if (g_bindings_only.load(std::memory_order_relaxed)) g_modern.tab = kControlsTab;
        input::ReleaseCursor();
    }
}

void CloseSettings() {
    if (PanelOpen() && !g_paper.load(std::memory_order_relaxed)) g_close_request.store(true, std::memory_order_relaxed);
    else ToggleMenu();
}

std::atomic<bool> g_open_request{false};
void OpenSettings() { g_open_request.store(true, std::memory_order_relaxed); }

void OpenBindings() {
    if (MenuOpen()) return;
    g_bindings_only.store(true, std::memory_order_relaxed);
    ToggleMenu();
}

void OpenPcOptions() {
    if (MenuOpen()) return;
    g_page = {};
    g_paper.store(true, std::memory_order_relaxed);
    ToggleMenu();
}

bool PcOptionsOpen() { return PanelOpen() && g_paper.load(std::memory_order_relaxed); }

void ExitToDesktop() {
    if (HWND window = static_cast<HWND>(Window())) PostMessageW(window, WM_CLOSE, 0, 0);
}

void PcOptionsPad(const input::Pad& pad) {
    uint32_t bits = pad.buttons & 0xF;  // dpad up, down, left, right
    constexpr int16_t kStick = 16000;
    if (pad.ly > kStick) bits |= kPadUp;
    if (pad.ly < -kStick) bits |= kPadDown;
    if (pad.lx < -kStick) bits |= kPadLeft;
    if (pad.lx > kStick) bits |= kPadRight;
    if (pad.analog[0] > 0x40) bits |= kPadA;
    if (pad.analog[1] > 0x40) bits |= kPadB;
    if (pad.analog[2] > 0x40) bits |= kPadX;
    if (pad.analog[3] > 0x40) bits |= kPadY;
    if (pad.analog[5] > 0x40 || pad.analog[6] > 0x40) bits |= kPadPrev;  // LB (white), the left trigger
    if (pad.analog[4] > 0x40 || pad.analog[7] > 0x40) bits |= kPadNext;  // RB (black), the right trigger
    g_paper_pad.store(pad.connected ? bits : 0, std::memory_order_relaxed);
}

void DrawUi(double fps) {
    ImGuiIO& io = ImGui::GetIO();
    // the arrow is ImGui's own, drawn only while the panel is open; Windows' stays hidden
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableKeyboard;
    if (g_open_request.exchange(false, std::memory_order_relaxed) && !MenuOpen()) ToggleMenu();
    bool paper = PcOptionsOpen();
    io.MouseDrawCursor = console::Open() && !PanelOpen();  // the settings menu has the game's own pointer
    io.KeyRepeatRate = 0.1f;
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (game::Opt().fps_counter) DrawFpsCounter(fps);
    DrawProfileMessage();
    // the port's version, in the menus (the front end also plays the logos and cinematics: not over those)
    if (game::cheats::InMenus() && !game::cheats::MoviePlaying() && !paper && !MenuOpen()) {
        static const std::string version = std::string("Recomp Version: ") + update::kVersion;
        update::State state = update::Check();
        bool newer = state == update::State::kAvailable;
        ui::paper::Stamp(version.c_str(), newer ? "Update Available" : state == update::State::kLatest ? "Latest Version" : "",
                         newer ? 0.7f : 0.25f);
    }
    if (paper) {
        DrawPaper();
    } else if (PanelOpen()) {
        DrawModern();
    }
    console::Draw();
}

}  // namespace app
#endif

#ifndef _WIN32
#include <algorithm>
#include <cstdio>
#include <iterator>
// the harness: pages of lines that change, to drive the screen with
namespace app::pc {
namespace {
const char* const kTitles[] = {"PC Options", "Display", "Graphics", "Mouse", "Keyboard", "Gameplay", "Unsaved Changes"};
constexpr int kCounts[] = {5, 8, 4, 4, 12, 3, 0};  // the first page's: four pages and the note
constexpr int kAsk = 6, kKeys = 4, kGameplay = 5;
bool g_extra = false, g_done = false;
int g_page = 0, g_focus = 0, g_from = 1;
bool g_dirty = false;
uint32_t g_serial = 0;
int g_values[6][12] = {};
int g_slot = 0;
bool g_waiting = false;  // the controls page's line 1's key box, after a press, until the next
void Open(int page, int focus) {
    g_page = page, g_focus = focus, g_serial++;
    std::printf("pc options: page %s\n", kTitles[page]);
}
void ToMain(int focus) {
    if (g_extra) g_done = true;
    else Open(0, focus);
}
}
void OpenExtra() {
    g_extra = true, g_done = false, g_dirty = false;
    Open(kGameplay, 0);
}
bool ExtraDone() { return g_extra && g_done; }
std::vector<Line> Lines(int selected) {
    std::vector<Line> out;
    if (g_page == kAsk) {
        Line note, keep;
        note.label = "Changes are not applied yet.";
        note.note = true;
        keep.label = "Keep Editing";
        keep.link = true;
        return {note, keep, {"Discard Changes", "", true}, {"Apply Changes", "", true}};
    }
    if (g_page == 0) {
        for (int p = 1; p <= kKeys; p++) {
            Line l;
            l.label = kTitles[p];
            l.link = true;
            out.push_back(l);
        }
        Line note;
        note.label = kPanelNote;
        note.note = true;
        out.push_back(note);
    }
    for (int i = 0; g_page && i < kCounts[g_page]; i++)
        out.push_back({"Setting " + std::to_string(i + 1), "Value " + std::to_string(g_values[g_page][i])});
    if (g_page == kKeys) {
        out[0].label = "Horizontal sensitivity";  // a long label beside a long value: the value is fitted
        out[0].value = "1.0 (Default)";
        out[1].value = "Right Shift   Mouse Button 4";  // a binding's two keys, as long as they get
        out[1].binding = true;
        out[1].keys[0] = "Right Shift";
        out[1].keys[1] = "Mouse Button 4";
        out[1].slot = selected == 1 ? g_slot : -1;
        out[1].waiting = selected == 1 && g_waiting;
        if (out[1].waiting) out[1].keys[g_slot] = "Press a key";
    }
    out.push_back({"Reset to Defaults", "", true});
    out.push_back({g_page && !g_extra ? "Apply" : "Apply and Exit", "", true});
    return out;
}
std::string Title() { return kTitles[g_page]; }
bool Subpage() { return g_page != 0; }
uint32_t PageSerial() { return g_serial; }
int Focus() { return g_focus; }
bool Back() {
    if (!g_page) return false;
    if (g_page == kAsk) {
        Open(g_from, 0);
    } else if (g_dirty) {
        g_from = g_page;
        Open(kAsk, 1);
    } else {
        ToMain(g_page - 1);
    }
    return true;
}
void Step(int line, int dir) {
    if (g_page == kKeys && line == 1) g_slot = dir < 0 ? 0 : 1;
    else if (g_page && line >= 0 && line < kCounts[g_page]) g_values[g_page][line] += dir, g_dirty = true;
    std::printf("pc options: step %d %d\n", line, dir);
}
void Press(int line) {
    std::printf("pc options: press %d\n", line);
    if (g_page == kAsk) {
        if (line == 1) return Open(g_from, 0);
        if (line == 2 || line == 3) {
            std::printf("pc options: %s\n", line == 2 ? "discarded" : "applied");
            g_dirty = false;
            ToMain(g_from - 1);
        }
        return;
    }
    int count = kCounts[g_page];
    if (!g_page && line >= 0 && line < kKeys) Open(line + 1, 0);
    else if (!g_page && line < count) return;
    else if (line == count) std::fill(std::begin(g_values[g_page]), std::end(g_values[g_page]), 0);
    else if (line == count + 1 && g_page) g_dirty = false, ToMain(g_page - 1);
    else if (g_page == kKeys && line == 1) g_waiting = !g_waiting;
    else if (line >= 0 && line < count) g_values[g_page][line]++, g_dirty = true;
}
void Clear(int line) { std::printf("pc options: clear %d\n", line); }
void Update() {}
bool Capturing() { return false; }
void Leave(bool applied) {
    g_extra = g_done = false;
    g_page = g_focus = 0;
    g_serial++;
    std::printf("pc options: leave, applied %d\n", int(applied));
}
}  // namespace app::pc

namespace app {
void OpenBindings() {}
void OpenPcOptions() {}
void ExitToDesktop() {}
bool PcOptionsOpen() { return false; }
void CloseSettings() {}
void PcOptionsPad(const input::Pad&) {}
}  // namespace app
#endif
