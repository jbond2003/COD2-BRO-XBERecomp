// the settings panel on F10, after BLACK-XBERecomp's
#include "common/menu.h"

float app::g_zoom = 1.0f;

#ifdef _WIN32
#include <algorithm>
#include <atomic>
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
#include "common/mouse_keyboard.h"
#include "common/options.h"

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
const char* const kDumpFormatNames[] = {"PNG", "DDS"};
// 30 keeps the game's own limiter
constexpr uint32_t kFpsCaps[] = {30, 60};  // limited to 60 for now

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
             kTextureReplacement, kTextureDumping, kDumpFormat, kMouseX, kMouseY, kInvertY, kInvertX, kQwerty };

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
    {kFpsCounter, "FPS COUNTER"},
    {kSection, "GRAPHICS"},
    {kAniso, "ANISOTROPIC FILTERING"},
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
};

bool DisplayRow(RowId id) { return id == kDisplayMode || id == kResolution; }

// display mode and resolution wait for Apply, the rest take effect at once
Settings g_held;
bool g_held_set = false;

Settings& Shown(RowId id) { return g_held_set && DisplayRow(id) ? g_held : Get(); }

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }

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
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            std::snprintf(text, sizeof(text), cap == 30 ? "30 (ORIGINAL)" : "%u", cap);
            break;
        }
        case kFov:
        {
            uint32_t& fov = game::Opt().fov;
            if (step) fov = uint32_t(StepRange(int(fov), int(game::kMinFov), int(game::kMaxFov), 1, step));
            std::snprintf(text, sizeof(text), fov == game::kDefaultFov ? "%u (DEFAULT)" : "%u", fov);
        }
            break;
        case kVsync:
            if (step) s.vsync = !s.vsync;
            return OnOff(s.vsync);
        case kFpsCounter:
            if (step) game::Opt().fps_counter = !game::Opt().fps_counter;
            return OnOff(game::Opt().fps_counter);
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
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
            float& v = id == kMouseX ? game::Opt().mouse_sensitivity : game::Opt().mouse_vertical_sensitivity;
            if (step) v = float(StepRange(int(v * 10.0f + 0.5f), 1, 50, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), v == 1.0f ? "%.1f (DEFAULT)" : "%.1f", v);
            break;
        }
        case kInvertY:
            if (step) game::Opt().invert_mouse = !game::Opt().invert_mouse;
            return OnOff(game::Opt().invert_mouse);
        case kInvertX:
            if (step) game::Opt().invert_mouse_x = !game::Opt().invert_mouse_x;
            return OnOff(game::Opt().invert_mouse_x);
        case kQwerty:
            if (step) game::Opt().qwerty_typewriter = !game::Opt().qwerty_typewriter;
            return game::Opt().qwerty_typewriter ? "QWERTY" : "ABC (ORIGINAL)";
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

void Defaults() {
    Settings d;
    Settings& s = Get();
    s.render_percent = d.render_percent;
    s.fps_cap = 60;  // 30 or 60 only for now
    s.vsync = false;
    s.aniso = d.aniso;
    s.custom_textures = d.custom_textures;
    s.dump_textures = d.dump_textures;
    s.texture_dump_format = d.texture_dump_format;
    game::Options& o = game::Opt();
    o.fps_counter = false;
    o.fov = game::kDefaultFov;
    o.mouse_sensitivity = 1.0f;
    o.mouse_vertical_sensitivity = 1.0f;
    o.invert_mouse = false;
    o.invert_mouse_x = false;
    o.qwerty_typewriter = true;
    g_held = s;
    g_held.display_mode = d.display_mode;
    g_held.width = d.width;
    g_held.height = d.height;
    g_held_set = true;
    Save();
}

void DrawRows() {
    for (const Row& r : kRows) {
        if (r.id == kSection) {
            Section(r.label);
            continue;
        }
        int step = OptionRow(r.label, RowText(r.id, 0, Shown(r.id)).c_str());
        if (!step) continue;
        if (DisplayRow(r.id)) {
            if (!g_held_set) g_held = Get();
            g_held_set = true;
            RowText(r.id, step, g_held);
        } else {
            RowText(r.id, step, Get());
            Save();
        }
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

// the key a waiting box takes: none until every key is up, then the first one down; Escape cancels
void UpdateCapture() {
    if (g_capture.action < 0) return;
    int vk = input::PressedKey();
    if (!g_capture.released) {
        g_capture.released = vk == 0;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.action), vk, g_capture.slot);
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

void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    bool only = g_bindings_only.load(std::memory_order_relaxed);
    ImGui::TextUnformatted(g_held_set ? "Display changes take effect on Apply" :
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
    ImGui::BeginDisabled(!g_held_set);
    if (ImGui::Button("Apply", ImVec2(bw, 0))) Apply();
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
        ImGui::EndChild();
        DrawFooter(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
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

}  // namespace


bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }

bool Capturing() { return g_capture.action >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    if (!open) g_bindings_only.store(false, std::memory_order_relaxed);
    g_held_set = false;
    g_capture = {};
    if (open) input::ReleaseCursor();
}

void OpenBindings() {
    if (MenuOpen()) return;
    g_bindings_only.store(true, std::memory_order_relaxed);
    ToggleMenu();
}

void DrawUi(double fps) {
    ImGuiIO& io = ImGui::GetIO();
    // the arrow is ImGui's own, drawn only while the panel is open; Windows' stays hidden
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableKeyboard;
    io.MouseDrawCursor = MenuOpen();
    io.KeyRepeatRate = 0.1f;
    io.FontGlobalScale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    if (game::Opt().fps_counter) DrawFpsCounter(fps);
    if (MenuOpen()) {
        UpdateCapture();
        DrawPanel();
    }
}

}  // namespace app
#endif

#ifndef _WIN32
namespace app {
void OpenBindings() {}
}  // namespace app
#endif
