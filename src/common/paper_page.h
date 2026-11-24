#pragma once
#include <string>
#include <vector>

#include <imgui.h>

// PC Options drawn as one of the game's own option pages: a sheet of paper in a dark frame, its title over a double
// rule, a line per setting (name left, value between two arrows right) that is faint until selected, and the page's
// actions centred at the foot. Drawing only; menu.cpp keeps the settings and the input.
namespace ui::paper {

struct Line {
    enum Kind { kHeading, kOption, kBinding, kAction };
    Kind kind = kOption;
    std::string label;
    std::string value;   // kOption: the value; kBinding: the first key
    std::string value2;  // kBinding: the second key
    int slot = 0;        // kBinding: the key box the selection is on
    bool waiting = false;  // kBinding: that box waits for a key
};

// where each line was drawn, for the mouse; empty rects for lines out of view
struct Hit {
    ImVec2 min, max;              // the line
    ImVec2 left_min, left_max;    // kOption: the left arrow; kBinding: the first key's box
    ImVec2 right_min, right_max;  // kOption: the right arrow; kBinding: the second key's box
};

// before the font atlas is built: ImGui's own font first (the F10 panel's), then a serif face for the page
void LoadFonts();
// after the atlas is built (cursor::Init): the game's paper into it
void Upload();

// the page over the whole screen. selected is a line index; scroll is in rows, and with follow it is moved so the
// selection is in view. hits gets one entry per line
void Draw(const char* title, const std::vector<Line>& lines, int selected, float& scroll, bool follow,
          const char* note, std::vector<Hit>& hits);

// a small line in the page's face at the bottom right of the screen, heavy and a quarter there; under it, if
// there is one, a second line as see-through as under_alpha says
void Stamp(const char* line, const char* under = nullptr, float under_alpha = 0.25f);

// the list's rows in view, for the mouse wheel
int VisibleRows();

// for the other pages drawn on the same sheet (modern_menu.cpp): the page's face and whether it is a bold one, and
// the game's sheet picture in the font atlas (false when the game's files gave none). In that picture, 512 square,
// the sheet is 12..499 across and 81..430 down, its printed frame 97..407 down and the rule under the title at 143
ImFont* Face();
bool FaceBold();
// Hack, for the FPS counter and the console: null when the exe has none. Pushed with ImGui::PushFont its size is
// MonoSize(), the console's text size for the window's height
ImFont* Mono();
float MonoSize();
bool Sheet(ImTextureID& texture, ImVec2& uv0, ImVec2& uv1);
// a controller picture in the atlas, 512 square: the game's Xbox controller, or the Modern configuration's
bool Pad(bool modern, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1);
// a controller's button as the game shows it elsewhere, in the atlas: set 0 its own icon, 1 the Modern
// configuration's picture, 2 that for a PlayStation pad; aspect is its width over its height. False when the game's
// files or the exe gave none
enum Button { kButtonA, kButtonB, kButtonX, kButtonY, kButtonLB, kButtonRB, kButtonLT, kButtonRT, kButtons };
bool Button(int button, int set, ImTextureID& texture, ImVec2& uv0, ImVec2& uv1, float& aspect);

}  // namespace ui::paper
