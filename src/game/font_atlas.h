// A game font sheet drawn again from a font file at a multiple of its size. The game's text comes from small bitmap
// fonts, a sheet texture plus a .fdf glyph table each. Typeset lays the font out as itself: one size for every letter
// (the original's capital height), the font's own widths and spacing, a table and a sheet of our own for the game to
// read in place of its own. Without that table (Draw without a layout) every glyph is stretched into the box the
// game's table gives it. After sharp_text in CoD2-BRO-Native-Loader (GPL-2.0-or-later, see THIRD_PARTY_NOTICES.md).
// Include after imstb_truetype.h.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace game::font_atlas {

// a line of a .fdf table, " 65 x 226 y 47 w 30 gx 1 gy 7 gw 28 gh 24": the glyph's box in the sheet (x, y, gw, gh,
// shadow included), its advance and its offset in the cell
struct Glyph {
    int code, x, y, advance, offset_x, offset_y, width, height;
};

inline std::vector<Glyph> ParseFdf(const std::vector<uint8_t>& data) {
    std::vector<Glyph> glyphs;
    const std::string text(data.begin(), data.end());
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        Glyph g;
        if (std::sscanf(line.c_str(), " %d x %d y %d w %d gx %d gy %d gw %d gh %d", &g.code, &g.x, &g.y, &g.advance,
                        &g.offset_x, &g.offset_y, &g.width, &g.height) == 8)
            glyphs.push_back(g);
    }
    return glyphs;
}

// the tables use Windows-1252 codes (0x8C = OE, 0x9C = oe)
inline int UnicodeOf(int code) {
    static constexpr uint16_t k80[32] = {0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                         0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
                                         0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                         0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
    return code >= 0x80 && code < 0xA0 ? k80[code - 0x80] : code;
}

struct Sheet {
    uint32_t width = 0, height = 0;  // the original sheet
    std::vector<Glyph> glyphs;
    bool shadow = false;      // a one pixel drop shadow to the lower right, inside the glyph boxes
    uint32_t shadow_rgb = 0;
};

constexpr float kWiden = 1.1f;  // the most a font narrower than the original is widened

// a font laid out as itself over a sheet the original's size
struct Layout {
    std::vector<Glyph> glyphs;  // the table for the game, as many glyphs and the codes of the original
    float sx = 0, sy = 0;       // sheet pixels per font unit
    int baseline = 0;           // in the cell, from its top
    std::string fdf;            // the original table's text with the glyph lines ours
};

inline int Median(std::vector<int> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// false when the font lacks capitals or its glyphs do not fit the sheet
inline bool Typeset(const Sheet& sheet, const std::vector<uint8_t>& fdf, const std::vector<uint8_t>& ttf, Layout& out) {
    stbtt_fontinfo info;
    if (ttf.empty() || !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return false;
    const int shadow = sheet.shadow ? 1 : 0;
    // the original's capital height and baseline, from the flat capitals; its width from the letters and digits
    std::vector<int> heights, baselines;
    double original_width = 0, font_width = 0;
    int x0, y0, x1, y1, cap = 0;
    if (!stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &cap) || cap <= 0) return false;
    for (const Glyph& g : sheet.glyphs) {
        if (std::string("BDEFHIKLMNPRTXZ").find(char(g.code)) != std::string::npos && g.code < 128) {
            heights.push_back(g.height - shadow);
            baselines.push_back(g.offset_y + g.height - shadow);
        }
        bool letter = (g.code >= 'A' && g.code <= 'Z') || (g.code >= 'a' && g.code <= 'z') || (g.code >= '0' && g.code <= '9');
        int advance, bearing;
        if (!letter || !stbtt_FindGlyphIndex(&info, g.code)) continue;
        stbtt_GetCodepointHMetrics(&info, g.code, &advance, &bearing);
        original_width += g.advance;
        font_width += advance;
    }
    if (heights.empty() || font_width <= 0) return false;
    out.sy = float(Median(heights)) / float(cap);
    out.baseline = Median(baselines);
    // as wide as the original where the font is the wider one, so every line still fits its place; a narrower font
    // is widened a little at most, it keeps its own look
    out.sx = out.sy * std::min(float(original_width / (font_width * out.sy)), kWiden);

    struct Box {
        size_t glyph;
        int w, h;
    };
    std::vector<Box> boxes;
    out.glyphs = sheet.glyphs;
    for (size_t i = 0; i < out.glyphs.size(); i++) {
        Glyph& g = out.glyphs[i];
        const int index = stbtt_FindGlyphIndex(&info, UnicodeOf(g.code));
        int advance = 0, bearing = 0;
        const bool ink = index && stbtt_GetGlyphBox(&info, index, &x0, &y0, &x1, &y1) && x1 > x0 && y1 > y0;
        if (index) {
            stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
            g.advance = std::max(1, int(std::lround(advance * out.sx)));
        }
        // no ink: an empty pixel of the sheet, the corner kept clear
        g.x = g.y = 0;
        g.width = g.height = 1;
        g.offset_x = 0;
        g.offset_y = out.baseline;
        if (!ink) continue;
        g.offset_x = int(std::floor(x0 * out.sx));
        g.offset_y = out.baseline + int(std::floor(-y1 * out.sy));
        g.width = int(std::ceil(x1 * out.sx)) - g.offset_x + shadow;
        g.height = out.baseline + int(std::ceil(-y0 * out.sy)) - g.offset_y + shadow;
        boxes.push_back({i, g.width, g.height});
    }
    // shelves, tallest first, a pixel between the boxes
    std::stable_sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) { return a.h > b.h; });
    int x = 2, y = 2, shelf = 0;
    for (const Box& b : boxes) {
        if (x + b.w + 1 > int(sheet.width)) {
            x = 2;
            y += shelf + 1;
            shelf = 0;
        }
        if (x + b.w + 1 > int(sheet.width) || y + b.h + 1 > int(sheet.height)) return false;
        out.glyphs[b.glyph].x = x;
        out.glyphs[b.glyph].y = y;
        x += b.w + 1;
        shelf = std::max(shelf, b.h);
    }

    // the table's text: its glyph lines again with our numbers
    out.fdf.clear();
    const std::string text(fdf.begin(), fdf.end());
    size_t next = 0;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        end = end == std::string::npos ? text.size() : end + 1;
        const std::string line = text.substr(start, end - start);
        start = end;
        int v[8];
        if (std::sscanf(line.c_str(), " %d x %d y %d w %d gx %d gy %d gw %d gh %d", &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6], &v[7]) != 8 ||
            next >= out.glyphs.size() || out.glyphs[next].code != v[0]) {
            out.fdf += line;
            continue;
        }
        const Glyph& g = out.glyphs[next++];
        char mine[96];
        std::snprintf(mine, sizeof(mine), "%3d x %d y %d w %d gx %d gy %d gw %d gh %d\r\n", g.code, g.x, g.y, g.advance,
                      g.offset_x, g.offset_y, g.width, g.height);
        out.fdf += mine;
    }
    return next == out.glyphs.size();
}

// the sheet at scale times its size as 0xAARRGGBB rows, white glyphs over their shadow; empty when ttf is no font.
// With a layout the font as Typeset laid it out, else each glyph stretched into the original table's box. missing
// counts the table's glyphs the font does not have (left empty).
inline std::vector<uint32_t> Draw(const Sheet& sheet, const std::vector<uint8_t>& ttf, int scale, int* missing = nullptr,
                                  const Layout* layout = nullptr) {
    stbtt_fontinfo info;
    if (ttf.empty() || !stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) return {};
    const uint32_t width = sheet.width * scale, height = sheet.height * scale;
    std::vector<uint8_t> coverage(size_t(width) * height, 0);
    std::vector<uint8_t> bitmap;
    int lost = 0;
    for (const Glyph& g : layout ? layout->glyphs : sheet.glyphs) {
        // the ink: the box without the shadow's extra pixel
        const int ink_w = g.width - (sheet.shadow ? 1 : 0), ink_h = g.height - (sheet.shadow ? 1 : 0);
        if (g.x < 0 || g.y < 0 || uint32_t(g.x + g.width) > sheet.width || uint32_t(g.y + g.height) > sheet.height)
            continue;
        const int index = stbtt_FindGlyphIndex(&info, UnicodeOf(g.code));
        int x0, y0, x1, y1;
        if (!index || !stbtt_GetGlyphBox(&info, index, &x0, &y0, &x1, &y1) || x1 <= x0 || y1 <= y0) {
            lost += !index;
            continue;
        }
        if (ink_w <= 0 || ink_h <= 0) continue;
        const int target_w = ink_w * scale, target_h = ink_h * scale;
        // stretched: scaled so the ink fills the original's box exactly, the game's spacing stays
        float sx = float(target_w) / float(x1 - x0), sy = float(target_h) / float(y1 - y0);
        // shifts that put the ink's left and top edge on the bitmap's first pixel
        float shift_x = std::ceil(x0 * sx) - x0 * sx, shift_y = std::ceil(-y1 * sy) + y1 * sy;
        int at_x = 0, at_y = 0;  // the bitmap in the box
        if (layout) {
            sx = layout->sx * scale, sy = layout->sy * scale;
            shift_x = shift_y = 0;
        }
        int bx0, by0, bx1, by1;
        stbtt_GetGlyphBitmapBoxSubpixel(&info, index, sx, sy, shift_x, shift_y, &bx0, &by0, &bx1, &by1);
        if (layout) {
            // the font's own place under the pen, to the part of a pixel
            at_x = bx0 - g.offset_x * scale;
            at_y = by0 + (layout->baseline - g.offset_y) * scale;
        }
        const int bw = bx1 - bx0, bh = by1 - by0;
        if (bw <= 0 || bh <= 0 || at_x < 0 || at_y < 0) continue;
        bitmap.assign(size_t(bw) * bh, 0);
        stbtt_MakeGlyphBitmapSubpixel(&info, bitmap.data(), bw, bh, bw, sx, sy, shift_x, shift_y, index);
        for (int y = 0; y < std::min(bh, target_h - at_y); y++)
            for (int x = 0; x < std::min(bw, target_w - at_x); x++) {
                uint8_t& out = coverage[size_t(g.y * scale + at_y + y) * width + g.x * scale + at_x + x];
                out = std::max(out, bitmap[size_t(y) * bw + x]);
            }
    }
    if (missing) *missing = lost;

    // white glyphs over the shadow (the glyphs again, scale pixels to the lower right)
    std::vector<uint32_t> argb(coverage.size());
    const float shadow[3] = {float(sheet.shadow_rgb >> 16 & 0xFF), float(sheet.shadow_rgb >> 8 & 0xFF),
                             float(sheet.shadow_rgb & 0xFF)};
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const size_t i = size_t(y) * width + x;
            const float g = coverage[i] / 255.0f;
            float s = 0.0f;
            if (sheet.shadow && x >= uint32_t(scale) && y >= uint32_t(scale))
                s = coverage[i - size_t(scale) * width - scale] / 255.0f * (1.0f - g);
            const float alpha = g + s;
            uint32_t pixel = uint32_t(std::lround(alpha * 255.0f)) << 24;
            for (int c = 0; c < 3; c++) {
                // transparent texels white, so filtering does not darken the edges
                const float value = alpha > 0.0f ? (255.0f * g + shadow[c] * s) / alpha : 255.0f;
                pixel |= uint32_t(std::lround(value)) << (16 - 8 * c);
            }
            argb[i] = pixel;
        }
    return argb;
}

}  // namespace game::font_atlas
