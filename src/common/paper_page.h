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

// the page over the whole screen. selected is a line index; scroll is in rows, and with follow it is moved so the
// selection is in view. hits gets one entry per line
void Draw(const char* title, const std::vector<Line>& lines, int selected, float& scroll, bool follow,
          const char* note, std::vector<Hit>& hits);

// the list's rows in view, for the mouse wheel
int VisibleRows();

}  // namespace ui::paper
