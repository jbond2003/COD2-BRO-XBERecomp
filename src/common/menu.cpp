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
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/paper_page.h"

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
             kTextureReplacement, kTextureDumping, kDumpFormat, kMouseX, kMouseY, kInvertY, kInvertX, kQwerty, kPrompts };

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
    {kPrompts, "BUTTON PROMPTS"},
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
        case kPrompts: {
            static constexpr const char* kModes[] = {"AUTOMATIC", "KEYBOARD AND MOUSE", "CONTROLLER"};
            uint32_t& m = game::Opt().button_prompts;
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
    o.button_prompts = 0;
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

}  // namespace


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
namespace app {
void OpenBindings() {}
void OpenPcOptions() {}
bool PcOptionsOpen() { return false; }
void PcOptionsPad(const input::Pad&) {}
}  // namespace app
#endif
