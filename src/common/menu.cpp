// the settings panel on F10, after BLACK-XBERecomp's
#include "common/menu.h"

float app::g_zoom = 1.0f;

#ifdef _WIN32
#include <algorithm>
#include <atomic>
#include <cctype>
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
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/paper_page.h"
#include "common/update_check.h"

namespace app {

namespace {

std::atomic<bool> g_open{false};
std::atomic<bool> g_bindings_only{false};  // opened from PC Options: the key bindings alone

// brass over dark olive, after the game's own menus
constexpr ImVec4 kPanelBg{0.04f, 0.05f, 0.07f, 0.94f};
constexpr ImVec4 kBorder{0.66f, 0.48f, 0.24f, 0.85f};
constexpr ImVec4 kText{0.92f, 0.89f, 0.82f, 1.0f};
constexpr ImVec4 kHighlight{0.30f, 0.20f, 0.09f, 1.0f};
constexpr ImVec4 kHeading{0.93f, 0.66f, 0.26f, 1.0f};
constexpr ImVec4 kDim{0.62f, 0.60f, 0.55f, 1.0f};
constexpr ImVec4 kFrame{0.10f, 0.11f, 0.14f, 1.0f};
constexpr ImVec4 kButton{0.16f, 0.13f, 0.09f, 1.0f};
constexpr ImVec4 kButtonHovered{0.42f, 0.29f, 0.12f, 1.0f};
constexpr ImVec4 kLine{0.93f, 0.66f, 0.26f, 0.35f};
constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kFrame}, {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kViewDistances[] = {1, 2, 4, 8};
const char* const kViewDistanceNames[] = {"GAME DEFAULT", "2x", "4x", "8x"};
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
             kBrightness, kAimAssist, kHints, kHideHud, kViewDistance };

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
    {kSection, "GRAPHICS"},
    {kAniso, "ANISOTROPIC FILTERING"},
    {kViewDistance, "VIEW DISTANCE"},
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
            std::snprintf(text, sizeof(text), "%ux%u", m.width, m.height);
            break;
        }
        case kRenderResolution: {
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
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
            if (step) b = uint32_t(StepRange(int(b), 50, 200, 10, step));
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
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            break;
        case kViewDistance:
            std::snprintf(text, sizeof(text), "%s", kViewDistanceNames[StepTable(kViewDistances, O().view_distance, step)]);
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
            if (step) v = float(StepRange(int(v * 10.0f + 0.5f), 1, 50, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), v == 1.0f ? "%.1f (DEFAULT)" : "%.1f", v);
            break;
        }
        case kInvertY:
            if (step) O().invert_mouse = !O().invert_mouse;
            return OnOff(O().invert_mouse);
        case kInvertX:
            if (step) O().invert_mouse_x = !O().invert_mouse_x;
            return OnOff(O().invert_mouse_x);
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

void Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
}

constexpr float kValueEms = 12.0f;  // the value column's width

// label left, an arrow each side of the value right; the row is the only nav stop. Returns -1, 0 or 1
int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize();
    float row_w = ImGui::GetContentRegionAvail().x;
    float row_h = ImGui::GetFrameHeight();
    float value_w = em * kValueEms;
    float block_w = row_h * 2 + value_w + style.ItemSpacing.x * 2;
    float x = ImGui::GetCursorPosX();
    float y = ImGui::GetCursorPosY();
    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(row_w, row_h));
    if (ImGui::IsItemFocused()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(ImGuiCol_Header));
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) step = 1;
    }
    ImGui::SetCursorPos(ImVec2(x + style.ItemInnerSpacing.x, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    float block_x = x + row_w - block_w;
    ImGui::SetCursorPos(ImVec2(block_x, y));
    if (ImGui::ArrowButton("##dec", ImGuiDir_Left)) step = -1;
    float value_x = block_x + row_h + style.ItemSpacing.x;
    float pad = (value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);
    ImGui::SetCursorPos(ImVec2(value_x + value_w + style.ItemSpacing.x, y));
    if (ImGui::ArrowButton("##inc", ImGuiDir_Right)) step = 1;
    ImGui::PopItemFlag();
    ImGui::PopID();
    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::Dummy(ImVec2(row_w, row_h));
    return step;
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
constexpr int kDisplayPart = 1, kGraphicsPart = 2, kMousePart = 4, kKeysPart = 8, kGameplayPart = 16, kEveryPart = 31;
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
    }
    if (parts & kGameplayPart) {
        o.aim_assist = true;
        o.hints = true;
        o.hide_hud = false;
    }
    if (parts & kGraphicsPart) {
        s.aniso = d.aniso;
        o.view_distance = 1;
        s.custom_textures = d.custom_textures;
        s.dump_textures = d.dump_textures;
        s.texture_dump_format = d.texture_dump_format;
    }
    if (parts & kMousePart) {
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
    o.view_distance = from_o.view_distance;
}

// one set of the rows' values differs from another
bool RowsDiffer(const Settings& a, const game::Options& c, const Settings& b, const game::Options& d) {
    return a.display_mode != b.display_mode || a.width != b.width || a.height != b.height ||
           a.render_percent != b.render_percent || a.fps_cap != b.fps_cap || a.vsync != b.vsync || a.aniso != b.aniso ||
           a.custom_textures != b.custom_textures || a.dump_textures != b.dump_textures ||
           a.texture_dump_format != b.texture_dump_format || c.fps_counter != d.fps_counter || c.fov != d.fov ||
           c.mouse_sensitivity != d.mouse_sensitivity || c.mouse_vertical_sensitivity != d.mouse_vertical_sensitivity ||
           c.invert_mouse != d.invert_mouse || c.invert_mouse_x != d.invert_mouse_x ||
           c.qwerty_typewriter != d.qwerty_typewriter || c.button_prompts != d.button_prompts ||
           c.brightness != d.brightness || c.aim_assist != d.aim_assist || c.hints != d.hints ||
           c.hide_hud != d.hide_hud || c.view_distance != d.view_distance;
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
}

// the F10 panel's Defaults: held like any other change, until Apply
void Defaults() {
    HoldPanel();
    SetDefaults(g_panel_settings, g_panel_options, kEveryPart);
}

void DrawRows() {
    for (const Row& r : kRows) {
        if (r.id == kSection) {
            Section(r.label);
            continue;
        }
        int step = OptionRow(r.label, PanelText(r.id, 0).c_str());
        if (step) PanelText(r.id, step);
    }
}

// a binding box waiting for a key: the action and slot, the keys let go of first
struct Capture {
    int action = -1;
    int slot = 0;
    bool released = false;
};
Capture g_capture;

// one action: its label, then a box for each of its two keys. A click waits for a key, right click clears
void BindingRow(size_t action, const char* label) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float em = ImGui::GetFontSize();
    float row_w = ImGui::GetContentRegionAvail().x;
    float box_w = em * 7.5f;
    ImGui::PushID(int(action));
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    for (int slot = 0; slot < 2; slot++) {
        ImGui::SameLine(row_w - box_w * float(2 - slot) - style.ItemSpacing.x * float(1 - slot));
        bool waiting = g_capture.action == int(action) && g_capture.slot == slot;
        std::string key = waiting ? "PRESS A KEY" : input::Binding(action, slot);
        if (key.empty()) key = "-";
        ImGui::PushID(slot);
        if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, kHighlight);
        if (ImGui::Button(key.c_str(), ImVec2(box_w, 0)) && !waiting) g_capture = {int(action), slot, false};
        if (waiting) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (waiting) g_capture = {};
            input::Bind(action, 0, slot);
        }
        ImGui::PopID();
    }
    ImGui::PopID();
}

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

void DrawBindings() {
    Section("CONTROLS");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("The keys do the same whichever controller layout is picked in the game's options. In menus "
                       "the arrows, Enter and Backspace move, pick and go back. Click a box and press a key, right "
                       "click to clear it.");
    ImGui::PopStyleColor();
    size_t count = 0;
    input::Actions(&count);
    for (size_t i = 0; i < count; i++) BindingRow(i, input::Label(i));
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    if (ImGui::Button("Reset controls", ImVec2(em * 10, 0))) {
        g_capture = {};
        input::ResetBindings();
    }
}

// Cheats (src/game/cheats.cpp): a small box for the code, and with the right one typed the engine's own cheats
void DrawCheats() {
    namespace cheats = game::cheats;
    float em = ImGui::GetFontSize();
    Section("CHEATS");
    if (!cheats::Unlocked()) {
        static char code[16] = "";
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Enter password");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 8);
        ImGui::InputText("##cheatcode", code, sizeof(code), ImGuiInputTextFlags_Password);
        if (cheats::TryUnlock(code)) code[0] = 0;
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("Cheats are on until the game is closed. They can break missions: use them at your own risk.");
    ImGui::PopStyleColor();
    static constexpr struct {
        cheats::Toggle toggle;
        const char* label;
    } kToggleRows[] = {{cheats::kGod, "God mode"},
                     {cheats::kNoTarget, "Enemies ignore you"},
                     {cheats::kLowGravity, "Low gravity (you only)"},
                     {cheats::kFastRun, "Run fast"},
                     {cheats::kBackwards, "sdrawkcaB"}};
    // ticked here, in force on Apply like the settings above
    HeldCheats& held = g_held_cheats;
    for (const auto& row : kToggleRows) {
        bool on = held.pending ? held.on[row.toggle] : cheats::On(row.toggle);
        if (ImGui::Checkbox(row.label, &on)) {
            HoldCheats();
            held.on[row.toggle] = on;
        }
    }
    bool ammo = held.pending ? held.ammo : cheats::UnlimitedAmmo();
    if (ImGui::Checkbox("Unlimited ammo", &ammo)) {
        HoldCheats();
        held.ammo = ammo;
    }
    bool heal = held.pending && held.heal;
    if (ImGui::Checkbox("Full health (once, on Apply)", &heal)) {
        HoldCheats();
        held.heal = heal;
    }
}

void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    bool only = g_bindings_only.load(std::memory_order_relaxed);
    ImGui::TextUnformatted(PanelChanged() ? "Changes take effect on Apply" :
                           only ? "Escape or Back returns to PC Options" : "F10 or Escape closes");
    ImGui::PopStyleColor();
    float bw = em * 8;
    float pad = ImGui::GetContentRegionAvail().x - bw * 3 - ImGui::GetStyle().ItemSpacing.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(pad, 0.0f));
    if (ImGui::Button("Back", ImVec2(bw, 0))) ToggleMenu();
    if (only) return;
    ImGui::SameLine();
    if (ImGui::Button("Defaults", ImVec2(bw, 0))) Defaults();
    ImGui::SameLine();
    bool changed = PanelChanged();
    ImGui::BeginDisabled(!changed);
    if (ImGui::Button("Apply", ImVec2(bw, 0))) ApplyPanel();
    ImGui::EndDisabled();
}

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(em * 44, std::min(em * 32, vp->Size.y * 0.92f)), ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        bool only = g_bindings_only.load(std::memory_order_relaxed);
        const char* title = only ? "KEY BINDINGS" : "SETTINGS";
        float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        if (!only) DrawRows();
        DrawBindings();
        if (!only) DrawCheats();
        ImGui::EndChild();
        DrawFooter(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

// ---- the PC Options page: the same settings on one of the game's own paper option pages (paper_page.h) ----

std::atomic<bool> g_paper{false};
std::atomic<uint32_t> g_paper_pad{0};  // the controller as kPad* bits, from the game thread's poll

constexpr uint32_t kPadUp = 1, kPadDown = 2, kPadLeft = 4, kPadRight = 8, kPadA = 0x100, kPadB = 0x200,
                   kPadX = 0x400;

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
std::string Sentence(const std::string& text) {
    static constexpr const char* kKeep[] = {"PNG", "DDS", "QWERTY", "ABC", "FPS", "FOV", "D-PAD"};
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
    for (char& c : out)
        if (std::isalpha(static_cast<unsigned char>(c))) {
            c = char(std::toupper(static_cast<unsigned char>(c)));
            break;
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

// top right, over the game
void DrawFpsCounter(double fps) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f FPS", fps);
    ImVec2 size = ImGui::CalcTextSize(text);
    ImVec2 pos(vp->Pos.x + vp->Size.x - size.x - em, vp->Pos.y + em * 0.6f);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), text);
    list->AddText(pos, ImGui::GetColorU32(kHeading), text);
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
          kFpsCounter},
         false},
        {"Graphics", kGraphicsPart, {kAniso, kViewDistance, kTextureReplacement, kTextureDumping, kDumpFormat}, false},
        {"Mouse", kMousePart, {kMouseX, kMouseY, kInvertY, kInvertX}, false},
        {"Keyboard", kKeysPart, {kPrompts, kQwerty}, true},
        {"Gameplay", kGameplayPart, {kAimAssist, kHints, kHideHud}, false},
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

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture.action >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    if (!open) {
        g_bindings_only.store(false, std::memory_order_relaxed);
        g_paper.store(false, std::memory_order_relaxed);
    }
    g_held_set = false;
    g_panel_pending = false;
    g_held_cheats = {};
    g_capture = {};
    if (open) input::ReleaseCursor();
}

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

bool PcOptionsOpen() { return MenuOpen() && g_paper.load(std::memory_order_relaxed); }

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
    g_paper_pad.store(pad.connected ? bits : 0, std::memory_order_relaxed);
}

void DrawUi(double fps) {
    ImGuiIO& io = ImGui::GetIO();
    // the arrow is ImGui's own, drawn only while the panel is open; Windows' stays hidden
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableKeyboard;
    bool paper = PcOptionsOpen();
    io.MouseDrawCursor = MenuOpen() && !paper;  // the PC Options page has the game's own pointer
    io.KeyRepeatRate = 0.1f;
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (game::Opt().fps_counter) DrawFpsCounter(fps);
    DrawProfileMessage();
    // the port's version, in the menus
    if (game::cheats::InMenus() && !paper && !MenuOpen()) {
        static const std::string version = std::string("Recomp Version: ") + update::kVersion;
        update::State state = update::Check();
        bool newer = state == update::State::kAvailable;
        ui::paper::Stamp(version.c_str(), newer ? "Update Available" : state == update::State::kLatest ? "Latest Version" : "",
                         newer ? 0.7f : 0.25f);
    }
    if (paper) {
        DrawPaper();
    } else if (MenuOpen()) {
        UpdateCapture();
        DrawPanel();
    }
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
bool PcOptionsOpen() { return false; }
void PcOptionsPad(const input::Pad&) {}
}  // namespace app
#endif
