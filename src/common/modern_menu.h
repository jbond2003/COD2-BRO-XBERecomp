#pragma once
#include <string>
#include <vector>

#include <imgui.h>

// The settings menu on F10: the game's own option sheet (paper_page.h has the paper and the face) filling the screen,
// a row of tabs under its title rule, a line per setting with its value in a box (a choice between two arrows, a
// slider, or On and Off), a note about the selected line on the right, the page's two actions at the foot
// and the keys and buttons along the bottom of the screen. Drawing only; menu.cpp keeps the settings and the input.
namespace ui::modern {

struct Row {
    enum Kind { kHeading, kChoice, kSlider, kToggle, kBinding, kEntry, kAction, kText };
    Kind kind = kChoice;
    std::string label;
    std::string value;   // kChoice, kSlider: the value; kBinding: the first key; kEntry: what is typed (shown as dots)
    std::string value2;  // kBinding: the second key
    float fill = 0;      // kSlider: 0 to 1
    bool on = false;     // kToggle
    bool changed = false;  // differs from what is in use: marked until Apply
    int slot = 0;          // kBinding: the key box the selection is on
    bool waiting = false;  // kBinding: that box waits for a key
};

struct Page {
    std::string title;
    std::string profile;  // the profile signed in, top right; empty for none
    std::vector<std::string> tabs;
    int tab = 0;
    std::vector<Row> rows;
    int selected = -1;
    bool follow = false;  // move the scroll so the selection is in view
    // the note about the selected line
    std::string note_title, note_text, note_command, note_change;
    int changes = 0;       // lines waiting for Apply
    bool controller = false;  // the bottom line shows the controller's buttons, else the keys
    std::string hint;      // a line over the foot, in place of the count of changes
};

struct Rect {
    ImVec2 min{0, 0}, max{0, 0};
    bool Has(ImVec2 p) const { return max.x > min.x && p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }
};

// where things were drawn, for the mouse; empty for what is out of view
struct Hits {
    struct Line {
        Rect row;
        Rect left, right;  // kChoice: the arrows; kSlider: left and right of the handle; kToggle: On, Off;
                           // kBinding: the two key boxes
        Rect bar;          // kSlider: the whole bar
    };
    std::vector<Rect> tabs;
    Rect tab_prev, tab_next;
    std::vector<Line> rows;
    Rect defaults, apply;
};

void Draw(const Page& page, float& scroll, Hits& hits);
int VisibleRows();  // the list's rows in view, for the mouse wheel

}  // namespace ui::modern
