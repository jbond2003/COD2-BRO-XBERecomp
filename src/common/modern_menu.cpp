// See modern_menu.h.
#include "common/modern_menu.h"

// Windows only, like the panel it replaces; PAPER_PREVIEW builds it on Linux to look at it
#if defined(_WIN32) || defined(PAPER_PREVIEW)

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "common/paper_page.h"

namespace ui::modern {

namespace {

// The sheet picture's own measures (paper_page.h): everything here is placed in that picture's pixels, so the page
// sits in the sheet's printed frame at any window size.
constexpr float kSheetLeft = 12, kSheetTop = 81, kSheetRight = 499, kSheetBottom = 430;
constexpr float kFrameLeft = 33, kFrameRight = 478, kFrameTop = 100, kFrameBottom = 404, kRuleY = 143;

constexpr ImVec4 kInk{0.14f, 0.11f, 0.08f, 1.0f};
constexpr ImVec4 kRed{0.47f, 0.11f, 0.08f, 1.0f};
constexpr ImVec4 kCream{0.92f, 0.88f, 0.78f, 1.0f};
constexpr float kIdle = 0.62f;  // an unselected line's ink

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }
ImU32 Ink(float alpha = 1.0f) { return Col(kInk, alpha); }
ImU32 Red(float alpha = 1.0f) { return Col(kRed, alpha); }

int g_rows = 8;

struct Canvas {
    ImDrawList* dl;
    ImVec2 o;      // the picture's corner on the screen
    float sx, sy;  // screen pixels a picture pixel
    float X(float tx) const { return std::floor(o.x + tx * sx); }
    float Y(float ty) const { return std::floor(o.y + ty * sy); }
    ImVec2 P(float tx, float ty) const { return ImVec2(X(tx), Y(ty)); }
    float Line(float t) const { return std::max(1.0f, std::floor(t * sy)); }

    ImVec2 Measure(float size, const std::string& text) const {
        return paper::Face()->CalcTextSizeA(size * sy, 1e9f, 0.0f, text.c_str());
    }
    // text of size (picture pixels) centred on the height ty; align 0 left, 0.5 centred, 1 right of tx; shrunk to
    // fit max_w (picture pixels) when it has one
    void Text(float size, float tx, float ty, ImU32 color, const std::string& text, float align = 0.0f,
              float max_w = 0) const {
        float px = size * sy;
        ImVec2 m = paper::Face()->CalcTextSizeA(px, 1e9f, 0.0f, text.c_str());
        if (max_w > 0 && m.x > max_w * sx) {
            px *= max_w * sx / m.x;
            m = paper::Face()->CalcTextSizeA(px, 1e9f, 0.0f, text.c_str());
        }
        ImVec2 pos(std::floor(o.x + tx * sx - m.x * align), std::floor(o.y + ty * sy - m.y * 0.5f));
        dl->AddText(paper::Face(), px, pos, color, text.c_str());
        // the game's print is heavier than a plain face: drawn again a little to the right
        if (!paper::FaceBold()) dl->AddText(paper::Face(), px, ImVec2(pos.x + std::max(1.0f, px * 0.03f), pos.y), color, text.c_str());
    }
    void Box(float x0, float y0, float x1, float y1, ImU32 color, float thick = 0.6f) const {
        dl->AddRect(P(x0, y0), P(x1, y1), color, 0.0f, 0, Line(thick));
    }
    void Fill(float x0, float y0, float x1, float y1, ImU32 color) const { dl->AddRectFilled(P(x0, y0), P(x1, y1), color); }
    void Arrow(float tx, float ty, float s, int dir, ImU32 color) const {
        float d = float(dir) * s * sy;
        ImVec2 c = P(tx, ty);
        dl->AddTriangleFilled(ImVec2(c.x + d * 0.55f, c.y), ImVec2(c.x - d * 0.45f, c.y - s * sy * 0.6f),
                              ImVec2(c.x - d * 0.45f, c.y + s * sy * 0.6f), color);
    }
};

// headings in capitals, spaced out, as the sheet's small print
std::string Spaced(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        out += char(std::toupper(static_cast<unsigned char>(text[i])));
        if (i + 1 < text.size()) out += ' ';
    }
    return out;
}

float RowHeight(const Row& r) { return r.kind == Row::kHeading ? 13.0f : r.kind == Row::kText ? 15.0f : 19.0f; }

// a key's cap, or a controller's button, on the line along the bottom of the screen; returns its width
float Cap(ImDrawList* dl, ImVec2 at, float h, const char* text, ImU32 fill, ImU32 ink, bool round) {
    float size = h * 0.62f;
    ImVec2 m = paper::Face()->CalcTextSizeA(size, 1e9f, 0.0f, text);
    float w = round ? h : std::max(h, m.x + h * 0.7f);
    ImVec2 a(std::floor(at.x), std::floor(at.y - h / 2)), b(std::floor(at.x + w), std::floor(at.y + h / 2));
    if (round) dl->AddCircleFilled(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), h / 2, fill, 32);
    else dl->AddRectFilled(a, b, fill, h * 0.14f);
    dl->AddText(paper::Face(), size, ImVec2(std::floor((a.x + b.x - m.x) / 2), std::floor((a.y + b.y - m.y) / 2)), ink, text);
    return w;
}

}  // namespace

int VisibleRows() { return g_rows; }

void Draw(const Page& page, float& scroll, Hits& hits) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    hits = {};
    hits.rows.assign(page.rows.size(), Hits::Line{});
    dl->AddRectFilled(ImVec2(0, 0), ds, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.72f)));

    // the sheet, nearly the whole screen, the line of keys under it
    ImVec2 p0(ds.x * 0.045f, ds.y * 0.03f), p1(ds.x * 0.955f, ds.y * 0.905f);
    Canvas c{dl, {}, (p1.x - p0.x) / (kSheetRight - kSheetLeft), (p1.y - p0.y) / (kSheetBottom - kSheetTop)};
    c.o = ImVec2(p0.x - kSheetLeft * c.sx, p0.y - kSheetTop * c.sy);
    ImTextureID texture{};
    ImVec2 uv0, uv1;
    if (paper::Sheet(texture, uv0, uv1)) {
        dl->AddImage(texture, c.o, ImVec2(c.o.x + 512.0f * c.sx, c.o.y + 512.0f * c.sy), uv0, uv1, IM_COL32(214, 206, 192, 255));
    } else {
        // no picture from the game's files: a plain sheet with the same frame and rule
        dl->AddRectFilledMultiColor(p0, p1, IM_COL32(180, 168, 143, 255), IM_COL32(180, 168, 143, 255),
                                    IM_COL32(150, 138, 115, 255), IM_COL32(150, 138, 115, 255));
        c.Box(kFrameLeft - 3, kFrameTop - 3, kFrameRight + 3, kFrameBottom + 3, Ink(0.85f), 1.4f);
        c.Fill(kFrameLeft, kRuleY - 0.6f, kFrameRight, kRuleY + 0.6f, Ink(0.85f));
    }

    // the title over the rule, the profile at its right
    c.Text(30.0f, kFrameLeft + 9, 122.0f, Ink(0.95f), page.title, 0.0f, 250);
    if (!page.profile.empty()) {
        c.Text(6.2f, kFrameRight - 5, 112.0f, Ink(0.65f), Spaced("Profile"), 1.0f);
        c.Text(13.0f, kFrameRight - 5, 127.0f, Ink(0.92f), page.profile, 1.0f, 150);
    }

    // the tabs under the rule, a key each end
    {
        const float y0 = 149.0f, y1 = 165.0f, key_w = 12.0f, gap = 2.5f;
        float left = kFrameLeft + 5, right = kFrameRight - 5;
        auto key = [&](float x0, const char* text, Rect& hit) {
            c.Fill(x0, y0 + 2, x0 + key_w, y1 - 2, Col(kCream, 0.35f));
            c.Box(x0, y0 + 2, x0 + key_w, y1 - 2, Ink(0.9f), 0.7f);
            c.Text(7.5f, x0 + key_w / 2, (y0 + y1) / 2, Ink(0.9f), text, 0.5f, key_w - 2);
            hit = {c.P(x0, y0), c.P(x0 + key_w, y1)};
        };
        key(left, page.controller ? "LB" : "Q", hits.tab_prev);
        key(right - key_w, page.controller ? "RB" : "E", hits.tab_next);
        float t0 = left + key_w + gap * 1.5f, t1 = right - key_w - gap * 1.5f;
        int n = std::max(1, int(page.tabs.size()));
        float tw = (t1 - t0 - gap * float(n - 1)) / float(n);
        hits.tabs.resize(page.tabs.size());
        for (int i = 0; i < int(page.tabs.size()); i++) {
            float x0 = t0 + (tw + gap) * float(i), x1 = x0 + tw;
            bool on = i == page.tab;
            if (on) {
                c.Fill(x0, y0, x1, y1, Ink(0.95f));
                c.Fill(x0, y1, x1, y1 + 1.2f, Red());
            } else {
                c.Fill(x0, y0, x1, y1, Col(kCream, 0.13f));
                c.Box(x0, y0, x1, y1, Ink(0.5f), 0.6f);
            }
            c.Text(10.5f, (x0 + x1) / 2, (y0 + y1) / 2, on ? Col(kCream) : Ink(0.6f), page.tabs[size_t(i)], 0.5f, tw - 4);
            hits.tabs[size_t(i)] = {c.P(x0, y0), c.P(x1, y1)};
        }
    }

    // the list, scrolled by whole rows to keep the selection in view
    const float list_left = kFrameLeft + 5, list_right = 302.0f, list_top = 171.0f, list_bottom = 376.0f;
    const float value_w = 112.0f, value_x1 = list_right - 3, value_x0 = value_x1 - value_w;
    int count = int(page.rows.size());
    auto fits_from = [&](int first) {  // how many rows fit from first
        float y = list_top;
        int n = 0;
        for (int i = first; i < count; i++) {
            float h = RowHeight(page.rows[size_t(i)]);
            if (y + h > list_bottom) break;
            y += h;
            n++;
        }
        return n;
    };
    int first = std::clamp(int(scroll), 0, std::max(0, count - 1));
    // no further down than shows the last row
    while (first > 0 && fits_from(first - 1) >= count - (first - 1)) first--;
    if (page.follow && page.selected >= 0 && page.selected < count) {
        if (page.selected < first) first = page.selected;
        while (first < page.selected && page.selected >= first + fits_from(first)) first++;
        // a heading above the selection comes into view with it
        if (first > 0 && first == page.selected && page.rows[size_t(first - 1)].kind == Row::kHeading) first--;
    }
    scroll = float(first);
    int shown = fits_from(first);
    g_rows = std::max(1, shown);
    float y = list_top;
    for (int i = first; i < first + shown; i++) {
        const Row& r = page.rows[size_t(i)];
        float h = RowHeight(r), cy = y + h / 2;
        Hits::Line& hit = hits.rows[size_t(i)];
        bool on = i == page.selected;
        float a = on ? 1.0f : kIdle;
        if (r.kind == Row::kHeading) {
            float ty = y + h - 4.5f;
            // (in line with the settings' names under it)
            c.Text(6.4f, list_left + 4, ty, Red(0.95f), Spaced(r.label));
            float tw = c.Measure(6.4f, Spaced(r.label)).x / c.sx;
            c.Fill(list_left + 4 + tw + 4, ty - 0.2f, list_right, ty + 0.2f, Red(0.5f));
            y += h;
            continue;
        }
        if (r.kind == Row::kText) {
            c.Text(9.0f, list_left + 4, cy, Ink(0.75f), r.label, 0.0f, list_right - list_left - 8);
            y += h;
            continue;
        }
        hit.row = {c.P(list_left, y), c.P(list_right, y + h)};
        if (on) {
            c.Fill(list_left, y + 0.5f, list_right, y + h - 0.5f, Red(0.13f));
            c.Box(list_left, y + 0.5f, list_right, y + h - 0.5f, Red(0.95f), 0.7f);
        } else {
            c.Fill(list_left, y + h - 0.2f, list_right, y + h + 0.2f, Ink(0.2f));
        }
        if (r.changed) c.Text(11.0f, list_left - 4.5f, cy + 1.5f, Red(), "*", 0.5f);
        float by0 = cy - 7.0f, by1 = cy + 7.0f;
        if (r.kind == Row::kAction) {
            float tw = c.Measure(10.0f, r.label).x / c.sx + 14;
            float x0 = list_left + 4;
            c.Box(x0, by0, x0 + tw, by1, Ink(on ? 0.95f : 0.5f), 0.8f);
            c.Text(10.0f, x0 + tw / 2, cy, Ink(a), r.label, 0.5f);
            y += h;
            continue;
        }
        c.Text(11.5f, list_left + 4, cy, Ink(on ? 1.0f : 0.82f), r.label, 0.0f, value_x0 - list_left - 10);
        // the value is lit while left and right change it: a line that waits to be picked has it as the others
        bool live = on && !r.locked;
        ImU32 edge = live ? Red(0.95f) : Ink(0.5f);
        if (on && r.editing && (r.kind == Row::kChoice || r.kind == Row::kSlider || r.kind == Row::kToggle))
            c.Box(value_x0 - 2.5f, by0 - 2.0f, value_x1 + 2.5f, by1 + 2.0f, Red(0.95f), 1.2f);
        switch (r.kind) {
            case Row::kChoice: {
                c.Fill(value_x0, by0, value_x1, by1, Col(kCream, live ? 0.45f : 0.2f));
                c.Box(value_x0, by0, value_x1, by1, edge);
                c.Arrow(value_x0 + 6, cy, 3.2f, -1, live ? Red() : Ink(0.6f));
                c.Arrow(value_x1 - 6, cy, 3.2f, 1, live ? Red() : Ink(0.6f));
                c.Text(10.0f, (value_x0 + value_x1) / 2, cy, Ink(on ? 1.0f : 0.85f), r.value, 0.5f, value_w - 26);
                float mid = (value_x0 + value_x1) / 2;
                hit.left = {c.P(value_x0, y), c.P(mid, y + h)};
                hit.right = {c.P(mid, y), c.P(value_x1, y + h)};
                break;
            }
            case Row::kSlider: {
                float bar1 = value_x1 - 24, fill = std::clamp(r.fill, 0.0f, 1.0f);
                float at = value_x0 + (bar1 - value_x0) * fill;
                // the game's own bar: a rounded frame of two lines, dark as far as the value, a tick at the middle
                const float half = 5.6f, inset = 1.5f;
                float round = 2.2f * c.sy;
                ImVec2 f0 = c.P(value_x0, cy - half), f1 = c.P(bar1, cy + half);
                ImVec2 i0 = c.P(value_x0 + inset, cy - half + inset), i1 = c.P(bar1 - inset, cy + half - inset);
                dl->AddRectFilled(f0, f1, Ink(0.3f), round);
                dl->AddRectFilled(i0, i1, Ink(0.25f), round * 0.6f);
                float end = i0.x + (i1.x - i0.x) * fill;
                if (end > i0.x + 1) {
                    dl->PushClipRect(i0, ImVec2(end, i1.y), true);
                    dl->AddRectFilled(i0, i1, Ink(0.93f), round * 0.6f);
                    dl->PopClipRect();
                }
                dl->AddRect(f0, f1, Ink(on ? 0.95f : 0.75f), round, 0, c.Line(0.6f));
                dl->AddRect(i0, i1, Ink(0.6f), round * 0.6f, 0, c.Line(0.4f));
                float mid = std::floor((i0.x + i1.x) / 2);
                dl->AddRectFilled(ImVec2(mid, f0.y), ImVec2(mid + c.Line(0.5f), f1.y), Ink(0.55f));
                if (r.editing) dl->AddRectFilled(ImVec2(end - c.Line(0.7f), i0.y), ImVec2(end + c.Line(0.7f), i1.y), Red());
                c.Text(10.0f, value_x1, cy, Ink(on ? 1.0f : 0.85f), r.value, 1.0f, 22);
                hit.bar = {c.P(value_x0, y), c.P(bar1, y + h)};
                hit.left = {c.P(value_x0 - 4, y), c.P(at, y + h)};
                hit.right = {c.P(at, y), c.P(bar1 + 4, y + h)};
                break;
            }
            case Row::kToggle: {
                float mid = (value_x0 + value_x1) / 2, gap = 1.5f;
                for (int k = 0; k < 2; k++) {
                    float x0 = k ? mid + gap : value_x0, x1 = k ? value_x1 : mid - gap;
                    bool lit = (k == 0) == r.on;
                    if (lit) c.Fill(x0, by0, x1, by1, Ink(0.95f));
                    else c.Box(x0, by0, x1, by1, Ink(0.4f));
                    c.Text(10.0f, (x0 + x1) / 2, cy, lit ? Col(kCream) : Ink(0.5f), k ? "Off" : "On", 0.5f);
                    (k ? hit.right : hit.left) = Rect{c.P(x0, y), c.P(x1, y + h)};
                }
                break;
            }
            case Row::kBinding: {
                float mid = (value_x0 + value_x1) / 2, gap = 1.5f;
                for (int k = 0; k < 2; k++) {
                    float x0 = k ? mid + gap : value_x0, x1 = k ? value_x1 : mid - gap;
                    bool box_on = on && r.slot == k, waiting = box_on && r.waiting;
                    const std::string& key = k ? r.value2 : r.value;
                    c.Fill(x0, by0, x1, by1, waiting ? Red(0.2f) : Col(kCream, box_on ? 0.5f : 0.2f));
                    c.Box(x0, by0, x1, by1, box_on ? Red(0.95f) : Ink(0.45f), box_on ? 0.9f : 0.6f);
                    c.Text(9.0f, (x0 + x1) / 2, cy, waiting ? Red() : Ink(key.empty() ? 0.4f : 0.9f),
                           waiting ? "Press a key" : key.empty() ? "-" : key, 0.5f, x1 - x0 - 5);
                    (k ? hit.right : hit.left) = Rect{c.P(x0, y), c.P(x1, y + h)};
                }
                break;
            }
            case Row::kEntry: {
                c.Fill(value_x0, by0, value_x1, by1, Col(kCream, on ? 0.5f : 0.25f));
                c.Box(value_x0, by0, value_x1, by1, edge);
                // a dot a letter, on the middle of the box, and the caret after the last
                const float step = 6.0f;
                float x = value_x0 + 6;
                for (size_t k = 0; k < r.value.size() && x < value_x1 - 6; k++, x += step)
                    dl->AddCircleFilled(ImVec2(c.o.x + (x + 1.5f) * c.sx, c.o.y + cy * c.sy), 1.6f * c.sy, Ink(0.9f), 16);
                if (on && std::fmod(ImGui::GetTime(), 1.0) < 0.5) c.Fill(x, cy - 4.5f, x + 0.6f, cy + 4.5f, Ink(0.9f));
                break;
            }
            default:
                break;
        }
        y += h;
    }
    // a scroll mark when the list is longer than the page
    if (shown < count) {
        float x = list_right + 3.5f, top = list_top + 2, bottom = list_bottom - 2;
        c.Fill(x - 0.3f, top, x + 0.3f, bottom, Ink(0.25f));
        float len = (bottom - top) * float(shown) / float(count);
        float at = top + (bottom - top - len) * float(first) / float(std::max(1, count - shown));
        c.Fill(x - 1.0f, at, x + 1.0f, at + len, Ink(0.8f));
    }

    // the note about the selected line, a slip on the sheet
    if (!page.note_title.empty()) {
        const float n0 = 314.0f, n1 = kFrameRight - 5, top = 176.0f, pad = 8.0f;
        float wrap = (n1 - n0 - pad * 2) * c.sx, text_px = 9.6f * c.sy;
        ImVec2 body = paper::Face()->CalcTextSizeA(text_px, 1e9f, wrap, page.note_text.c_str());
        float height = 30.0f + body.y / c.sy + (page.note_change.empty() ? 0.0f : 12.0f) +
                       (page.note_command.empty() ? 0.0f : 26.0f) + 6.0f;
        float bottom = std::min(top + height, list_bottom);
        dl->AddRectFilled(ImVec2(c.X(n0) + c.Line(1.2f), c.Y(top) + c.Line(1.8f)),
                          ImVec2(c.X(n1) + c.Line(1.2f), c.Y(bottom) + c.Line(1.8f)), IM_COL32(0, 0, 0, 70));
        c.Fill(n0, top, n1, bottom, IM_COL32(238, 230, 208, 215));
        c.Box(n0, top, n1, bottom, Ink(0.35f), 0.4f);
        float ty = top + 16.0f;
        c.Text(14.0f, n0 + pad, ty, Red(), page.note_title, 0.0f, n1 - n0 - pad * 2);
        c.Fill(n0 + pad, ty + 8.5f, n1 - pad, ty + 9.1f, Red(0.6f));
        ImVec2 at(c.X(n0 + pad), c.Y(ty + 13.0f));
        dl->AddText(paper::Face(), text_px, at, Ink(0.88f), page.note_text.c_str(), nullptr, wrap);
        if (!paper::FaceBold()) dl->AddText(paper::Face(), text_px, ImVec2(at.x + 1, at.y), Ink(0.88f), page.note_text.c_str(), nullptr, wrap);
        float ny = ty + 13.0f + body.y / c.sy + 7.0f;
        if (!page.note_change.empty()) {
            c.Text(6.0f, n0 + pad, ny, Red(0.95f), Spaced(page.note_change), 0.0f, n1 - n0 - pad * 2);
            ny += 12.0f;
        }
        if (!page.note_command.empty()) {
            c.Text(5.6f, n0 + pad, ny, Ink(0.6f), Spaced("Console"));
            ImFont* mono = ImGui::GetIO().Fonts->Fonts[0];
            float mono_px = std::max(13.0f, std::round(9.0f * c.sy / 13.0f) * 13.0f);
            ImVec2 m = mono->CalcTextSizeA(mono_px, 1e9f, 0.0f, page.note_command.c_str());
            ImVec2 b0(c.X(n0 + pad), c.Y(ny + 5.5f)), b1(b0.x + m.x + c.sx * 5.0f, b0.y + m.y + c.sy * 3.0f);
            dl->AddRectFilled(b0, b1, Ink(0.9f));
            dl->AddText(mono, mono_px, ImVec2(std::floor(b0.x + c.sx * 2.5f), std::floor(b0.y + c.sy * 1.5f)), Col(kCream),
                        page.note_command.c_str());
        }
    }

    // the foot: Reset to Defaults, what waits, Apply Changes
    {
        const float y0 = 381.0f, y1 = 399.0f, cy = (y0 + y1) / 2;
        float dx0 = kFrameLeft + 5, dx1 = dx0 + 96;
        c.Box(dx0, y0, dx1, y1, Ink(0.6f), 1.0f);
        c.Text(11.0f, (dx0 + dx1) / 2, cy, Ink(0.75f), "Reset to Defaults", 0.5f, 88);
        hits.defaults = {c.P(dx0, y0), c.P(dx1, y1)};
        float ax1 = kFrameRight - 5, ax0 = ax1 - 90;
        bool waiting = page.changes > 0;
        // waiting: a solid red button lettered in cream, plain to see on the dark of the sheet's foot
        if (waiting) c.Fill(ax0, y0, ax1, y1, Red());
        c.Box(ax0, y0, ax1, y1, waiting ? Ink(0.9f) : Ink(0.45f), 1.0f);
        c.Text(11.0f, (ax0 + ax1) / 2, cy, waiting ? Col(kCream) : Ink(0.5f), "Apply Changes", 0.5f, 82);
        hits.apply = {c.P(ax0, y0), c.P(ax1, y1)};
        std::string middle = page.hint;
        if (middle.empty() && waiting)
            middle = "* " + std::to_string(page.changes) + (page.changes == 1 ? " change waiting" : " changes waiting");
        if (!middle.empty())
            c.Text(page.hint.empty() ? 7.0f : 9.0f, (dx1 + ax0) / 2, cy, page.hint.empty() ? Red() : Ink(0.8f),
                   page.hint.empty() ? Spaced(middle) : middle, 0.5f, ax0 - dx1 - 10);
    }

    // leaving with changes waiting: the sheet dimmed, a slip in its middle with the choices
    if (page.confirm) {
        c.Fill(kSheetLeft, kSheetTop, kSheetRight, kSheetBottom, IM_COL32(0, 0, 0, 120));
        const float w = 250.0f, h = 92.0f, mx = (kFrameLeft + kFrameRight) / 2, x0 = mx - w / 2, x1 = mx + w / 2;
        const float y0 = 205.0f, y1 = y0 + h;
        dl->AddRectFilled(ImVec2(c.X(x0) + c.Line(1.6f), c.Y(y0) + c.Line(2.4f)),
                          ImVec2(c.X(x1) + c.Line(1.6f), c.Y(y1) + c.Line(2.4f)), IM_COL32(0, 0, 0, 110));
        c.Fill(x0, y0, x1, y1, IM_COL32(238, 230, 208, 250));
        c.Box(x0, y0, x1, y1, Ink(0.6f), 0.8f);
        c.Text(17.0f, mx, y0 + 17.0f, Red(), "Unsaved Changes", 0.5f, w - 20);
        c.Fill(x0 + 12, y0 + 28.0f, x1 - 12, y0 + 28.6f, Red(0.6f));
        std::string text = page.changes == 1 ? "1 change has not been applied."
                                             : std::to_string(page.changes) + " changes have not been applied.";
        c.Text(10.5f, mx, y0 + 42.0f, Ink(0.9f), text, 0.5f, w - 20);
        int n = int(page.confirm_choices.size());
        const float gap = 6.0f, by0 = y1 - 30.0f, by1 = y1 - 11.0f;
        float bw = (w - 24.0f - gap * float(std::max(0, n - 1))) / float(std::max(1, n));
        hits.confirm.resize(size_t(n));
        for (int i = 0; i < n; i++) {
            float bx0 = x0 + 12.0f + (bw + gap) * float(i), bx1 = bx0 + bw;
            bool on = i == page.confirm_selected;
            if (on) c.Fill(bx0, by0, bx1, by1, Red(0.13f));
            c.Box(bx0, by0, bx1, by1, on ? Red() : Ink(0.5f), on ? 1.0f : 0.7f);
            c.Text(10.5f, (bx0 + bx1) / 2, (by0 + by1) / 2, on ? Red() : Ink(0.75f), page.confirm_choices[size_t(i)], 0.5f, bw - 6);
            hits.confirm[size_t(i)] = {c.P(bx0, by0), c.P(bx1, by1)};
        }
    }

    // along the bottom of the screen: the keys, or the controller's buttons
    {
        struct Item {
            const char* key;
            const char* key2;
            const char* button;
            ImU32 color;
            const char* text;
        };
        const Item items[] = {
            {"Enter", nullptr, "A", IM_COL32(63, 143, 58, 255), "SELECT"},
            {"Esc", nullptr, "B", IM_COL32(181, 56, 44, 255), "BACK"},
            {"Q", "E", "LB", 0, "TABS"},
            {"R", nullptr, "X", IM_COL32(47, 95, 174, 255), "DEFAULTS"},
            {"F", nullptr, "Y", IM_COL32(201, 162, 31, 255), "APPLY"},
        };
        float h = std::floor(ds.y * 0.034f), text_px = h * 0.66f, gap = h * 0.3f, between = h * 1.5f;
        ImU32 cream = Col(kCream), dark = Ink();
        auto width = [&](const Item& it) {
            float w = 0;
            auto cap = [&](const char* t, bool round) {
                ImVec2 m = paper::Face()->CalcTextSizeA(h * 0.62f, 1e9f, 0.0f, t);
                w += (round ? h : std::max(h, m.x + h * 0.7f)) + gap;
            };
            if (page.controller) {
                cap(it.button, it.color != 0);
                if (!it.color) cap("RB", false);
            } else {
                cap(it.key, false);
                if (it.key2) cap(it.key2, false);
            }
            return w + paper::Face()->CalcTextSizeA(text_px, 1e9f, 0.0f, it.text).x;
        };
        float total = -between;
        for (const Item& it : items) total += width(it) + between;
        float x = std::floor((ds.x - total) / 2), cy = std::floor(ds.y * 0.953f);
        for (const Item& it : items) {
            if (page.controller) {
                x += Cap(dl, ImVec2(x, cy), h, it.button, it.color ? it.color : cream, it.color ? IM_COL32_WHITE : dark, it.color != 0) + gap;
                if (!it.color) x += Cap(dl, ImVec2(x, cy), h, "RB", cream, dark, false) + gap;
            } else {
                x += Cap(dl, ImVec2(x, cy), h, it.key, cream, dark, false) + gap;
                if (it.key2) x += Cap(dl, ImVec2(x, cy), h, it.key2, cream, dark, false) + gap;
            }
            ImVec2 m = paper::Face()->CalcTextSizeA(text_px, 1e9f, 0.0f, it.text);
            dl->AddText(paper::Face(), text_px, ImVec2(x, std::floor(cy - m.y / 2)), cream, it.text);
            x += m.x + between;
        }
    }
}

}  // namespace ui::modern
#endif
