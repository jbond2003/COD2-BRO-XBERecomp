// See paper_page.h.
#include "common/paper_page.h"

// Windows only, like the panel; PAPER_PREVIEW builds it on Linux to look at it
#if defined(_WIN32) || defined(PAPER_PREVIEW)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include "core/log.h"

namespace game {
bool ReadFrontEndPicture(const std::string& name, std::vector<uint8_t>& rgba, uint32_t& width,
                         uint32_t& height);  // src/game/prompts.cpp
bool ReadEarlyGameFile(const std::string& xbox_path, std::vector<uint8_t>& out);  // src/game/prompts.cpp
}
#endif

namespace ui::paper {

namespace {

ImFont* g_serif = nullptr;
// the game's own option page (single_options_misc.dds of sp/frontend.cod, the Sound page's), in the font atlas
int g_paper_rect = -1;
std::vector<uint8_t> g_paper_pixels;
uint32_t g_paper_w = 0, g_paper_h = 0;
// the sheet in that picture (alpha over half), its printed frame's top and bottom lines and the rule under the title
constexpr float kSheetLeft = 12, kSheetTop = 81, kSheetRight = 499, kSheetBottom = 430;
constexpr float kRuleY = 143;
int g_rows = 8;

// the game's paper and ink
constexpr ImVec4 kBoard{0.20f, 0.17f, 0.13f, 1.0f};
constexpr ImVec4 kPaperTop{0.70f, 0.66f, 0.56f, 1.0f};
constexpr ImVec4 kPaperBottom{0.58f, 0.54f, 0.45f, 1.0f};
constexpr ImVec4 kInk{0.15f, 0.12f, 0.09f, 1.0f};
constexpr float kIdle = 0.50f;  // an unselected line's ink

ImU32 Ink(float alpha) { return ImGui::GetColorU32(ImVec4(kInk.x, kInk.y, kInk.z, alpha)); }
ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

ImFont* Serif() { return g_serif ? g_serif : ImGui::GetFont(); }

ImVec2 Measure(float size, const char* text) {
    return Serif()->CalcTextSizeA(size, 1e9f, 0.0f, text);
}

// text fitted into max_w: shrunk, never grown past size
void Text(ImDrawList* dl, float size, ImVec2 at, ImU32 color, const std::string& text, float max_w = 0,
          float align = 0.0f) {
    ImVec2 m = Measure(size, text.c_str());
    if (max_w > 0 && m.x > max_w) {
        size *= max_w / m.x;
        m = Measure(size, text.c_str());
    }
    ImVec2 pos(std::floor(at.x - m.x * align), std::floor(at.y - m.y * 0.5f));
    dl->AddText(Serif(), size, pos, color, text.c_str());
    // the game's print is heavier than the face: drawn again a little to the right
    if (g_paper_rect >= 0) dl->AddText(Serif(), size, ImVec2(pos.x + std::max(1.0f, size * 0.03f), pos.y), color, text.c_str());
}

// the game's own pages write their lines as titles ("Reset to Defaults"), ours are in capitals: as the game's
std::string Titled(const std::string& text) {
    static const char* const kKeep[] = {"PC", "FPS", "FOV", "HUD", "ABC", "QWERTY", "DDS", "PNG", "TGA", "MSAA", "VSYNC"};
    static const char* const kSmall[] = {"AND", "OF", "TO", "THE", "IN", "A", "OR", "PER"};
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i < text.size()) {
        size_t j = i;
        while (j < text.size() && std::isalpha(uint8_t(text[j]))) j++;
        if (j == i) {
            out += text[i++];
            continue;
        }
        std::string word = text.substr(i, j - i);
        bool keep = std::any_of(std::begin(kKeep), std::end(kKeep), [&](const char* k) { return word == k; });
        bool small = !first && std::any_of(std::begin(kSmall), std::end(kSmall), [&](const char* k) { return word == k; });
        bool caps = std::none_of(word.begin(), word.end(), [](char c) { return std::islower(uint8_t(c)); });
        if (caps && !keep && !(word.size() == 1 && !small)) {
            for (size_t k = 0; k < word.size(); k++)
                word[k] = char(k == 0 && !small ? std::toupper(uint8_t(word[k])) : std::tolower(uint8_t(word[k])));
        }
        out += word;
        first = false;
        i = j;
    }
    return out;
}

void Arrow(ImDrawList* dl, ImVec2 c, float s, int dir, ImU32 color) {
    float d = float(dir);
    dl->AddTriangleFilled(ImVec2(c.x + d * s * 0.55f, c.y), ImVec2(c.x - d * s * 0.45f, c.y - s * 0.6f),
                          ImVec2(c.x - d * s * 0.45f, c.y + s * 0.6f), color);
}

// a key's box on a binding line
void KeyBox(ImDrawList* dl, ImVec2 min, ImVec2 max, float u, const std::string& key, bool on, bool waiting,
            float alpha) {
    if (on) dl->AddRectFilled(min, max, Ink(0.12f * alpha / kIdle * kIdle));
    dl->AddRect(min, max, Ink((on ? 0.9f : 0.35f) * alpha), 0.0f, 0, std::max(1.0f, u * 0.18f));
    std::string text = waiting ? "press a key" : key.empty() ? "-" : key;
    Text(dl, u * 3.6f, ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2), Ink(alpha), text, (max.x - min.x) * 0.9f,
         0.5f);
}

}  // namespace

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty()) io.Fonts->AddFontDefault();
#ifdef _WIN32
    // the game's paper, and its typeface from the disc (media/GARA.TTF)
    if (game::ReadFrontEndPicture("single_options_misc.dds", g_paper_pixels, g_paper_w, g_paper_h))
        g_paper_rect = io.Fonts->AddCustomRectRegular(int(g_paper_w), int(g_paper_h));
    LOGR("PC Options: %s", g_paper_rect >= 0 ? "the game's paper" : "no paper from the game files, a drawn one");
    std::vector<uint8_t> ttf;
    if (game::ReadEarlyGameFile("D:\\media\\GARA.TTF", ttf) && ttf.size() > 1024) {
        void* data = IM_ALLOC(ttf.size());  // the atlas owns it
        std::memcpy(data, ttf.data(), ttf.size());
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        g_serif = io.Fonts->AddFontFromMemoryTTF(data, int(ttf.size()), 56.0f, &cfg);
        if (g_serif) return;
    }
#endif
    std::vector<std::string> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"GARABD.TTF", "GARA.TTF", "georgiab.ttf", "timesbd.ttf"}) candidates.push_back(fonts + f);
#else
    candidates = {"/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf"};
#endif
    for (const std::string& path : candidates) {
        if (!std::ifstream(path, std::ios::binary)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        g_serif = io.Fonts->AddFontFromFileTTF(path.c_str(), 56.0f, &cfg);
        if (g_serif) break;
    }
}

void Upload() {
    if (g_paper_rect < 0) return;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    unsigned char* texture = nullptr;
    int tw = 0, th = 0;
    atlas->GetTexDataAsRGBA32(&texture, &tw, &th);
    const ImFontAtlasCustomRect* rect = atlas->GetCustomRectByIndex(g_paper_rect);
    if (!texture || !rect->IsPacked()) {
        g_paper_rect = -1;
        return;
    }
    for (uint32_t y = 0; y < g_paper_h; y++)
        std::memcpy(texture + (size_t(rect->Y + y) * size_t(tw) + rect->X) * 4,
                    g_paper_pixels.data() + size_t(y) * g_paper_w * 4, size_t(g_paper_w) * 4);
    atlas->TexPixelsUseColors = true;
    g_paper_pixels.clear();
    g_paper_pixels.shrink_to_fit();
}

int VisibleRows() { return g_rows; }

void Draw(const char* title, const std::vector<Line>& lines, int selected, float& scroll, bool follow,
          const char* note, std::vector<Hit>& hits) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    hits.assign(lines.size(), Hit{});
    dl->AddRectFilled(ImVec2(0, 0), ds, ImGui::GetColorU32(ImVec4(0, 0, 0, 0.55f)));

    // the sheet where the game puts its own option pages
    ImVec2 p0(ds.x * 0.14f, ds.y * 0.155f), p1(ds.x * 0.86f, ds.y * 0.845f);
    float u = (p1.y - p0.y) / 100.0f;  // a hundredth of the sheet's height
    float w = p1.x - p0.x;
    float rule_y = p0.y + u * 19.0f;
    ImVec2 f0(p0.x + u * 4.2f, p0.y + u * 4.2f), f1(p1.x - u * 4.2f, p1.y - u * 4.2f);
    if (g_paper_rect >= 0) {
        // the game's own page, its sheet over p0..p1
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;
        ImVec2 uv0, uv1;
        atlas->CalcCustomRectUV(atlas->GetCustomRectByIndex(g_paper_rect), &uv0, &uv1);
        float sx = w / (kSheetRight - kSheetLeft), sy = (p1.y - p0.y) / (kSheetBottom - kSheetTop);
        ImVec2 o(p0.x - kSheetLeft * sx, p0.y - kSheetTop * sy);
        dl->AddImage(atlas->TexID, o, ImVec2(o.x + float(g_paper_w) * sx, o.y + float(g_paper_h) * sy), uv0, uv1);
        rule_y = o.y + kRuleY * sy;
    } else {
    dl->AddRectFilled(p0, p1, Col(kBoard));
    ImVec2 s0(p0.x + u * 1.6f, p0.y + u * 1.6f), s1(p1.x - u * 1.6f, p1.y - u * 1.6f);
    dl->AddRectFilledMultiColor(s0, s1, Col(kPaperTop), Col(kPaperTop), Col(kPaperBottom), Col(kPaperBottom));
    // folds and a little wear
    for (float f : {0.33f, 0.66f}) {
        float x = s0.x + (s1.x - s0.x) * f;
        dl->AddLine(ImVec2(x, s0.y), ImVec2(x, s1.y), Ink(0.06f), std::max(1.0f, u * 0.25f));
        dl->AddLine(ImVec2(x + u * 0.3f, s0.y), ImVec2(x + u * 0.3f, s1.y), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.05f)),
                    std::max(1.0f, u * 0.2f));
    }
    float fold_y = (s0.y + s1.y) / 2;
    dl->AddLine(ImVec2(s0.x, fold_y), ImVec2(s1.x, fold_y), Ink(0.05f), std::max(1.0f, u * 0.25f));
    // the printed frame: a heavy rule and a fine one inside it
    dl->AddRect(f0, f1, Ink(0.85f), 0.0f, 0, std::max(1.5f, u * 0.55f));
    dl->AddRect(ImVec2(f0.x + u * 0.9f, f0.y + u * 0.9f), ImVec2(f1.x - u * 0.9f, f1.y - u * 0.9f), Ink(0.45f), 0.0f,
                0, std::max(1.0f, u * 0.2f));
    dl->AddLine(ImVec2(f0.x + u * 1.5f, rule_y), ImVec2(f1.x - u * 1.5f, rule_y), Ink(0.85f), std::max(1.5f, u * 0.5f));
    dl->AddLine(ImVec2(f0.x + u * 1.5f, rule_y + u * 0.9f), ImVec2(f1.x - u * 1.5f, rule_y + u * 0.9f), Ink(0.5f),
                std::max(1.0f, u * 0.18f));
    }

    // the title over the rule
    float cx = (p0.x + p1.x) / 2;
    Text(dl, u * 10.0f, ImVec2(cx, (p0.y + u * 4.6f + rule_y) / 2), Ink(0.92f), title, w * 0.8f, 0.5f);

    // the list, scrolled to keep the selection in view; the actions stay at the foot
    std::vector<int> listed, actions;
    for (int i = 0; i < int(lines.size()); i++) (lines[size_t(i)].kind == Line::kAction ? actions : listed).push_back(i);
    float list_top = p0.y + u * 23.0f, row_h = u * 6.6f;
    float actions_top = p1.y - u * (9.0f + 6.6f * float(actions.size()));
    g_rows = std::max(1, int((actions_top - u * 2.0f - list_top) / row_h));
    int max_scroll = std::max(0, int(listed.size()) - g_rows);
    int sel_pos = int(std::find(listed.begin(), listed.end(), selected) - listed.begin());
    if (follow && sel_pos < int(listed.size())) {
        if (float(sel_pos) < scroll) scroll = float(sel_pos);
        if (float(sel_pos) > scroll + float(g_rows) - 1) scroll = float(sel_pos - g_rows + 1);
        // a heading above the selection comes into view with it
        if (sel_pos > 0 && lines[size_t(listed[size_t(sel_pos - 1)])].kind == Line::kHeading &&
            float(sel_pos - 1) < scroll)
            scroll = float(sel_pos - 1);
    }
    scroll = std::clamp(scroll, 0.0f, float(max_scroll));
    int first = int(scroll);
    float label_x = p0.x + w * 0.12f, value_cx = p0.x + w * 0.70f, value_w = w * 0.26f;
    for (int k = first; k < int(listed.size()) && k < first + g_rows; k++) {
        int i = listed[size_t(k)];
        const Line& l = lines[size_t(i)];
        float y = list_top + row_h * float(k - first) + row_h / 2;
        Hit& h = hits[size_t(i)];
        h.min = ImVec2(f0.x + u, y - row_h / 2);
        h.max = ImVec2(f1.x - u, y + row_h / 2);
        if (l.kind == Line::kHeading) {
            Text(dl, u * 3.4f, ImVec2(cx, y + u * 0.6f), Ink(0.62f), l.label, w * 0.8f, 0.5f);
            float tw = Measure(u * 3.4f, l.label.c_str()).x / 2 + u * 2.0f;
            dl->AddLine(ImVec2(label_x, y + u * 0.6f), ImVec2(cx - tw, y + u * 0.6f), Ink(0.3f), std::max(1.0f, u * 0.15f));
            dl->AddLine(ImVec2(cx + tw, y + u * 0.6f), ImVec2(p0.x + w * 0.88f, y + u * 0.6f), Ink(0.3f),
                        std::max(1.0f, u * 0.15f));
            h = Hit{};
            continue;
        }
        bool on = i == selected;
        float alpha = on ? 1.0f : kIdle;
        Text(dl, u * 5.2f, ImVec2(label_x, y), Ink(alpha), Titled(l.label), value_cx - value_w / 2 - label_x - u * 5.0f);
        if (l.kind == Line::kOption) {
            Text(dl, u * 4.6f, ImVec2(value_cx, y), Ink(alpha), Titled(l.value), value_w, 0.5f);
            float a = u * 2.4f, ax = value_w / 2 + u * 3.0f;
            Arrow(dl, ImVec2(value_cx - ax, y), a, -1, Ink(alpha));
            Arrow(dl, ImVec2(value_cx + ax, y), a, 1, Ink(alpha));
            h.left_min = ImVec2(value_cx - ax - a * 1.5f, y - row_h / 2);
            h.left_max = ImVec2(value_cx - ax + a * 1.5f, y + row_h / 2);
            h.right_min = ImVec2(value_cx + ax - a * 1.5f, y - row_h / 2);
            h.right_max = ImVec2(value_cx + ax + a * 1.5f, y + row_h / 2);
        } else {
            float bw = value_w * 0.62f, gap = u * 1.5f, bh = row_h * 0.72f;
            ImVec2 b0(value_cx - bw - gap / 2, y - bh / 2), b1(value_cx - gap / 2, y + bh / 2);
            ImVec2 c0(value_cx + gap / 2, y - bh / 2), c1(value_cx + bw + gap / 2, y + bh / 2);
            KeyBox(dl, b0, b1, u, l.value, on && l.slot == 0, on && l.waiting && l.slot == 0, alpha);
            KeyBox(dl, c0, c1, u, l.value2, on && l.slot == 1, on && l.waiting && l.slot == 1, alpha);
            h.left_min = b0, h.left_max = b1, h.right_min = c0, h.right_max = c1;
        }
    }
    // a scroll mark when the list is longer than the page
    if (max_scroll > 0) {
        float top = list_top, bottom = list_top + row_h * float(g_rows), x = f1.x - u * 2.6f;
        dl->AddLine(ImVec2(x, top), ImVec2(x, bottom), Ink(0.2f), std::max(1.0f, u * 0.3f));
        float len = (bottom - top) * float(g_rows) / float(listed.size());
        float at = top + (bottom - top - len) * scroll / float(max_scroll);
        dl->AddLine(ImVec2(x, at), ImVec2(x, at + len), Ink(0.75f), std::max(2.0f, u * 0.7f));
    }
    for (size_t k = 0; k < actions.size(); k++) {
        int i = actions[k];
        float y = actions_top + u * 6.6f * float(k) + u * 3.3f;
        bool on = i == selected;
        std::string label = Titled(lines[size_t(i)].label);
        Text(dl, u * 5.4f, ImVec2(cx, y), Ink(on ? 1.0f : kIdle), label, w * 0.7f, 0.5f);
        float tw = Measure(u * 5.4f, label.c_str()).x / 2 + u * 2.0f;
        hits[size_t(i)].min = ImVec2(cx - tw, y - u * 3.3f);
        hits[size_t(i)].max = ImVec2(cx + tw, y + u * 3.3f);
    }
    if (note && *note) Text(dl, u * 2.8f, ImVec2(cx, f1.y - u * 2.4f), Ink(0.6f), note, w * 0.8f, 0.5f);
}

}  // namespace ui::paper
#endif
