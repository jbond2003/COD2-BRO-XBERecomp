// Keyboard and mouse button prompts. The game shows its controller buttons as pictures: an icon font
// (i_button_icons_xb_<language>.dds with its .fdf table, codes 33-49: triggers, A B X Y, white and black, the dpad,
// start and back) and four footer icons (Xbox_A/B/X/YButton.dds), all in sp/global.cod. While the keyboard and mouse
// are in use the renderer draws keycaps and mouse pictures in their place (gpu/texture_override.h), labelled with the
// keys that give each button in the controller layout picked in the game's options; the controller's icons come back
// the moment a controller is used, with LB and RB in place of the original's white and black buttons. After
// CoD2-BRO-Native-Loader's text/key_prompts.cpp (GPL-2.0-or-later, THIRD_PARTY_NOTICES.md).

#include <algorithm>
#include <filesystem>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "gpu/texture.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"
#include "app/app.h"
#include "kernel/disc.h"

namespace game {
int PadTarget(size_t i);  // src/game/mouse_look.cpp
}

namespace {

using gpu::texture_override::Picture;

// the icon font's glyphs in its order (codes 33-49), and the footer icons' buttons
enum Icon {
    kRightTrigger, kA, kX, kY, kB, kLeftTrigger, kWhite, kBlack,
    kDpadUp, kDpadRight, kDpadLeft, kDpadDown, kStart, kDpadLeftRight, kDpadUpDown, kDpad, kBack,
    kIconCount
};
// a key's name ("F", "Space", "Esc"), or a picture: "@lmb" "@rmb" "@mmb" a mouse with that button lit, "@up" "@down"
// "@left" "@right" "@leftright" "@updown" "@arrows" arrow keys. Empty: an empty keycap.
using Labels = std::array<std::string, kIconCount>;
// labels[kA] saying this: a controller is in use, its pictures stay but for those labelled ("@lb" "@rb": the white
// and black buttons, a pad's LB and RB)
constexpr const char* kControllerSet = "@controller";

constexpr const char* kPack = "D:\\sp\\global.cod";
constexpr const char* kIconFonts[] = {"i_button_icons_xb_en", "i_button_icons_xb_fr", "i_button_icons_xb_it",
                                      "i_button_icons_xb_sp"};
constexpr std::pair<const char*, Icon> kFooterIcons[] = {
    {"Xbox_AButton.dds", kA}, {"Xbox_BButton.dds", kB}, {"Xbox_XButton.dds", kX}, {"Xbox_YButton.dds", kY}};
constexpr int kFirstIconCode = 33;  // '!' the right trigger, then Icon's order

struct Box {
    Icon icon;
    int x, y, width, height;  // in the original texture
};

// one icon texture: its level 0 bytes as the game loads them (what follows the DDS header) and where each icon is
struct IconTexture {
    std::string name;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> level0;
    std::vector<Box> boxes;
};

std::once_flag g_loaded;
std::vector<IconTexture> g_textures;  // written once, under g_loaded
// the main menu's paper (Single_Front_menu.dds in sp/frontend.cod) has four boxes printed on it, one a line: with
// Multiplayer gone there are three lines, so the fourth box is painted over with the paper beside it
std::vector<uint8_t> g_menu_paper;  // its level 0, written once under g_loaded
std::shared_ptr<const Picture> g_menu_paper_picture;  // under g_lock
std::vector<uint8_t> g_ttf;
stbtt_fontinfo g_font;
bool g_have_font = false;

std::mutex g_lock;
std::optional<Labels> g_labels;  // under g_lock
std::map<std::pair<const IconTexture*, int>, std::shared_ptr<const Picture>> g_cache;  // under g_lock

// --- The pack ("KAPF"): its tables come before the first chunk at 0x8000. From 0x28 one 24-byte record a section:
// u16 type, u16 file count, chunk table offset, directory offset. Directory: 20 bytes a file - name offset, path
// offset, size, chunk, offset in the chunk. Chunk table: 28-byte records starting with the chunk's offset.

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}
uint16_t U16(const std::vector<uint8_t>& d, size_t at) {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
}

struct Entry {
    uint64_t offset;
    uint32_t size;
};

// a part of a game file (len 0: all of it), also before boot, while D: is not mapped yet (the PC Options page reads
// its paper with the window): from the disc image, else from the assets folder
bool ReadAny(const std::string& xbox_path, uint64_t pos, uint32_t len, std::vector<uint8_t>& out) {
    if (len ? kernel::ReadGameFile(xbox_path, pos, len, out) : kernel::ReadGameFile(xbox_path, out)) return true;
    if (xbox_path.size() < 4 || (xbox_path[0] != 'D' && xbox_path[0] != 'd')) return false;
    std::string rel = xbox_path.substr(3);
    if (const kernel::Disc* disc = kernel::MountedDisc()) {
        const kernel::Disc::Entry* e = disc->Find(std::string_view(rel));
        if (!e || e->dir) return false;
        uint32_t n = len ? len : uint32_t(e->size);
        out.resize(n);
        return disc->Read(*e, pos, out.data(), n) == n;
    }
    std::filesystem::path dir = app::ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; i++) {
        if (std::filesystem::is_directory(dir / "assets", ec)) break;
        if (!dir.has_parent_path() || dir.parent_path() == dir) return false;
        dir = dir.parent_path();
    }
    std::string host = rel;
    std::replace(host.begin(), host.end(), '\\', '/');
    std::ifstream f(dir / "assets" / host, std::ios::binary);
    if (!f) return false;
    if (!len) {
        out.assign(std::istreambuf_iterator<char>(f), {});
        return !out.empty();
    }
    out.resize(len);
    if (!f.seekg(std::streamoff(pos))) return false;
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(len));
    return uint64_t(f.gcount()) == len;
}

std::map<std::string, Entry> ReadDirectory(const char* pack = kPack) {
    std::map<std::string, Entry> files;
    std::vector<uint8_t> h;
    if (!ReadAny(pack, 0, 0x8000, h) || std::memcmp(h.data(), "KAPF", 4) != 0) return files;
    auto string_at = [&](uint32_t at) {
        std::string s;
        while (at < h.size() && h[at]) s += char(h[at++]);
        return s;
    };
    uint32_t sections = U32(h, 0x0C);
    for (uint32_t i = 0; i < sections && i < 16; i++) {
        size_t record = 0x28 + size_t(i) * 24;
        uint16_t count = U16(h, record + 2);
        uint32_t chunks = U32(h, record + 4), directory = U32(h, record + 8);
        for (uint32_t e = 0; e < count; e++) {
            size_t at = directory + size_t(e) * 20;
            if (at + 20 > h.size()) break;
            size_t chunk = chunks + size_t(U32(h, at + 12)) * 28;
            if (chunk + 4 > h.size()) continue;
            files[string_at(U32(h, at))] = {uint64_t(U32(h, chunk)) + U32(h, at + 16), U32(h, at + 8)};
        }
    }
    return files;
}

// the glyph boxes of an icon font's table (" 33 x 7 y 10 w 30 gx 3 gy 8 gw 26 gh 26")
std::vector<Box> IconTable(const std::vector<uint8_t>& data) {
    std::vector<Box> boxes;
    std::string table(data.begin(), data.end());
    size_t start = 0;
    while (start < table.size()) {
        size_t end = table.find('\n', start);
        if (end == std::string::npos) end = table.size();
        std::string line = table.substr(start, end - start);
        start = end + 1;
        int code, x, y, advance, gx, gy, gw, gh;
        if (std::sscanf(line.c_str(), " %d x %d y %d w %d gx %d gy %d gw %d gh %d", &code, &x, &y, &advance, &gx, &gy,
                        &gw, &gh) != 8)
            continue;
        int index = code - kFirstIconCode;
        if (index < 0 || index >= kIconCount || gw <= 0 || gh <= 0) continue;
        boxes.push_back({Icon(index), x, y, gw, gh});
    }
    return boxes;
}

// the size of a DDS's level 0, 0 for a format the icons do not come in
uint32_t Level0Size(const std::vector<uint8_t>& dds, uint32_t width, uint32_t height) {
    uint32_t flags = U32(dds, 80);
    const char* fourcc = reinterpret_cast<const char*>(dds.data() + 84);
    uint32_t blocks = std::max(1u, (width + 3) / 4) * std::max(1u, (height + 3) / 4);
    if (flags & 4) {
        if (!std::memcmp(fourcc, "DXT1", 4)) return blocks * 8;
        if (!std::memcmp(fourcc, "DXT3", 4) || !std::memcmp(fourcc, "DXT5", 4)) return blocks * 16;
        return 0;
    }
    return width * height * (U32(dds, 88) / 8);
}

bool ReadFont() {
    // the game's own Garamond, the face its typewriter keys are lettered in
    if (kernel::ReadGameFile("D:\\media\\GARA.TTF", g_ttf) && !g_ttf.empty() &&
        stbtt_InitFont(&g_font, g_ttf.data(), stbtt_GetFontOffsetForIndex(g_ttf.data(), 0)))
        return true;
    std::vector<std::string> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"georgiab.ttf", "timesbd.ttf", "arialbd.ttf", "arial.ttf"}) candidates.push_back(fonts + f);
#else
    candidates = {"/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
                  "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                  "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"};
#endif
    for (const std::string& path : candidates) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        g_ttf.assign(std::istreambuf_iterator<char>(in), {});
        if (!g_ttf.empty() && stbtt_InitFont(&g_font, g_ttf.data(), stbtt_GetFontOffsetForIndex(g_ttf.data(), 0)))
            return true;
    }
    return false;
}

void Load() {
    g_have_font = ReadFont();
    if (!g_have_font) LOGW("button prompts: no font for the key names");
    std::map<std::string, Entry> files = ReadDirectory();
    auto read = [&](const std::string& name, std::vector<uint8_t>& out) {
        auto it = files.find(name);
        return it != files.end() && kernel::ReadGameFile(kPack, it->second.offset, it->second.size, out);
    };
    auto add = [&](const std::string& name, const std::vector<uint8_t>& dds, std::vector<Box> boxes) {
        if (dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || boxes.empty()) return;
        IconTexture t;
        t.name = name;
        t.height = U32(dds, 12);
        t.width = U32(dds, 16);
        uint32_t size = Level0Size(dds, t.width, t.height);
        if (!t.width || !t.height || t.width > 1024 || t.height > 1024 || !size || 128 + size > dds.size()) return;
        t.level0.assign(dds.begin() + 128, dds.begin() + 128 + size);
        t.boxes = std::move(boxes);
        g_textures.push_back(std::move(t));
    };
    std::vector<uint8_t> dds, table;
    for (const char* font : kIconFonts)
        if (read(std::string(font) + ".dds", dds) && read(std::string(font) + ".fdf", table))
            add(font, dds, IconTable(table));
    for (const auto& [name, icon] : kFooterIcons)
        if (read(name, dds) && dds.size() > 20)
            add(name, dds, {{icon, 0, 0, int(U32(dds, 16)), int(U32(dds, 12))}});
    LOGR("button prompts: %zu icon textures", g_textures.size());
    std::map<std::string, Entry> front = ReadDirectory("D:\\sp\\frontend.cod");
    if (auto it = front.find("Single_Front_menu.dds"); it != front.end() &&
        kernel::ReadGameFile("D:\\sp\\frontend.cod", it->second.offset, it->second.size, dds) &&
        dds.size() == 128 + 256 * 512 && U32(dds, 16) == 256 && U32(dds, 12) == 512)
        g_menu_paper.assign(dds.begin() + 128, dds.end());
}

// --- Drawing: straight alpha RGBA floats; shapes are signed distance fields in output pixels, anti-aliased over one.

struct Color {
    float r, g, b, a;
};

class Canvas {
public:
    Canvas(uint32_t width, uint32_t height) : width_(width), height_(height), pixels_(size_t(width) * height) {}

    // coverage (0-1) of color over what is there
    void Blend(int x, int y, Color c, float coverage) {
        if (x < 0 || y < 0 || x >= int(width_) || y >= int(height_)) return;
        float a = c.a * std::clamp(coverage, 0.0f, 1.0f);
        if (a <= 0.0f) return;
        Color& d = pixels_[size_t(y) * width_ + size_t(x)];
        float out = a + d.a * (1.0f - a);
        d.r = (c.r * a + d.r * d.a * (1.0f - a)) / out;
        d.g = (c.g * a + d.g * d.a * (1.0f - a)) / out;
        d.b = (c.b * a + d.b * d.a * (1.0f - a)) / out;
        d.a = out;
    }

    void Set(int x, int y, Color c) {
        if (x >= 0 && y >= 0 && x < int(width_) && y < int(height_)) pixels_[size_t(y) * width_ + size_t(x)] = c;
    }

    // fills where distance(px, py) (pixel centres) is negative
    template <typename Distance>
    void Fill(float left, float top, float right, float bottom, Color c, Distance distance) {
        for (int y = std::max(0, int(top) - 1); y < std::min(int(height_), int(bottom) + 2); y++)
            for (int x = std::max(0, int(left) - 1); x < std::min(int(width_), int(right) + 2); x++)
                Blend(x, y, c, 0.5f - distance(float(x) + 0.5f, float(y) + 0.5f));
    }

    std::shared_ptr<Picture> Finish() const {
        auto pic = std::make_shared<Picture>();
        pic->width = width_;
        pic->height = height_;
        pic->argb.resize(pixels_.size());
        for (size_t i = 0; i < pixels_.size(); i++) {
            const Color& c = pixels_[i];
            // transparent texels white, so filtering does not darken the edges
            auto channel = [&](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            if (c.a <= 0.0f) {
                pic->argb[i] = 0x00FFFFFF;
                continue;
            }
            pic->argb[i] = channel(c.a) << 24 | channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
        }
        return pic;
    }

private:
    uint32_t width_, height_;
    std::vector<Color> pixels_;
};

float RoundedRect(float px, float py, float cx, float cy, float half_w, float half_h, float radius) {
    float qx = std::abs(px - cx) - half_w + radius, qy = std::abs(py - cy) - half_h + radius;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - radius;
}

// a triangle pointing (dx, dy) (unit, along an axis), centred on (cx, cy)
void Arrow(Canvas& canvas, float cx, float cy, float size, int dx, int dy, Color c) {
    float tx = cx + float(dx) * size * 0.5f, ty = cy + float(dy) * size * 0.5f;
    float bx = cx - float(dx) * size * 0.4f, by = cy - float(dy) * size * 0.4f;
    float ax = bx - float(dy) * size * 0.5f, ay = by + float(dx) * size * 0.5f;
    float ex = bx + float(dy) * size * 0.5f, ey = by - float(dx) * size * 0.5f;
    const float xs[3] = {tx, ax, ex}, ys[3] = {ty, ay, ey};
    canvas.Fill(cx - size, cy - size, cx + size, cy + size, c, [&](float px, float py) {
        float d = -1e9f;
        for (int i = 0; i < 3; i++) {  // the largest signed distance to the three edges, outside positive
            float x0 = xs[i], y0 = ys[i], x1 = xs[(i + 1) % 3], y1 = ys[(i + 1) % 3];
            float nx = y1 - y0, ny = x0 - x1, length = std::hypot(nx, ny);
            float side = ((px - x0) * nx + (py - y0) * ny) / length;
            float centre = ((cx - x0) * nx + (cy - y0) * ny) / length;
            if (centre > 0) side = -side;
            d = std::max(d, side);
        }
        return d;
    });
}

void Text(Canvas& canvas, float cx, float cy, float max_w, float max_h, const std::string& text, Color c) {
    if (!g_have_font || text.empty()) return;
    // the size that fits: the cap height fills max_h unless the text is too wide
    float scale = stbtt_ScaleForPixelHeight(&g_font, max_h * 1.35f);
    auto measure = [&](float s, int& x0, int& y0, int& x1, int& y1) {
        float pen = 0;
        x0 = y0 = 1 << 20;
        x1 = y1 = -(1 << 20);
        for (size_t i = 0; i < text.size(); i++) {
            int code = static_cast<unsigned char>(text[i]);
            int bx0, by0, bx1, by1, advance, bearing;
            stbtt_GetCodepointBitmapBoxSubpixel(&g_font, code, s, s, pen - std::floor(pen), 0, &bx0, &by0, &bx1, &by1);
            x0 = std::min(x0, int(std::floor(pen)) + bx0);
            x1 = std::max(x1, int(std::floor(pen)) + bx1);
            y0 = std::min(y0, by0);
            y1 = std::max(y1, by1);
            stbtt_GetCodepointHMetrics(&g_font, code, &advance, &bearing);
            pen += float(advance) * s;
            if (i + 1 < text.size())
                pen += s * float(stbtt_GetCodepointKernAdvance(&g_font, code, static_cast<unsigned char>(text[i + 1])));
        }
    };
    int x0, y0, x1, y1;
    measure(scale, x0, y0, x1, y1);
    scale *= std::min(max_w / float(std::max(1, x1 - x0)), max_h / float(std::max(1, y1 - y0)));
    measure(scale, x0, y0, x1, y1);
    float origin_x = cx - float(x0 + x1) / 2.0f, origin_y = cy - float(y0 + y1) / 2.0f;
    float pen = origin_x;
    std::vector<uint8_t> bitmap;
    for (size_t i = 0; i < text.size(); i++) {
        int code = static_cast<unsigned char>(text[i]);
        float shift_x = pen - std::floor(pen), shift_y = origin_y - std::floor(origin_y);
        int bx0, by0, bx1, by1;
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, code, scale, scale, shift_x, shift_y, &bx0, &by0, &bx1, &by1);
        int w = bx1 - bx0, h = by1 - by0;
        if (w > 0 && h > 0) {
            bitmap.assign(size_t(w) * size_t(h), 0);
            stbtt_MakeCodepointBitmapSubpixel(&g_font, bitmap.data(), w, h, w, scale, scale, shift_x, shift_y, code);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    canvas.Blend(int(std::floor(pen)) + bx0 + x, int(std::floor(origin_y)) + by0 + y, c,
                                 float(bitmap[size_t(y) * size_t(w) + size_t(x)]) / 255.0f);
        }
        int advance, bearing;
        stbtt_GetCodepointHMetrics(&g_font, code, &advance, &bearing);
        pen += float(advance) * scale;
        if (i + 1 < text.size())
            pen += scale * float(stbtt_GetCodepointKernAdvance(&g_font, code, static_cast<unsigned char>(text[i + 1])));
    }
}

// The look of the profile name typewriter's keys: round, a bright metal ring around a near black face, the name in
// off-white in the game's own Garamond (d:\media\GARA.TTF), a soft shadow below.
constexpr Color kInk{0.95f, 0.93f, 0.88f, 1.0f};
constexpr Color kShadow{0.0f, 0.0f, 0.0f, 0.55f};
constexpr Color kRingTop{0.88f, 0.87f, 0.84f, 1.0f};
constexpr Color kRingBottom{0.36f, 0.35f, 0.33f, 1.0f};
constexpr Color kFaceTop{0.27f, 0.26f, 0.25f, 1.0f};
constexpr Color kFaceBottom{0.07f, 0.07f, 0.07f, 1.0f};
constexpr Color kLit{0.80f, 0.16f, 0.15f, 1.0f};

Color Mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

// a shape filled top to bottom from one colour to another
template <typename Shape>
void Shade(Canvas& canvas, float top, float bottom, float left, float right, Color from, Color to, Shape shape) {
    for (int y = int(top) - 1; y <= int(bottom) + 1; y++) {
        Color c = Mix(from, to, std::clamp((float(y) + 0.5f - top) / std::max(1.0f, bottom - top), 0.0f, 1.0f));
        canvas.Fill(left, float(y), right, float(y), c, [&](float px, float py) {
            return std::max(shape(px, py), std::abs(py - (float(y) + 0.5f)) - 0.5f);
        });
    }
}

// a small mouse in ink with one button lit: 0 left, 1 right, 2 middle
void Mouse(Canvas& canvas, float cx, float cy, float size, int button) {
    float line = std::max(1.0f, size * 0.09f);
    float hw = size * 0.30f, hh = size * 0.44f, radius = hw * 0.95f;
    float split_y = cy - hh * 0.12f;  // where the buttons end
    canvas.Fill(cx - hw, cy - hh, cx + hw, split_y, kLit, [&](float px, float py) {
        float d = std::max(RoundedRect(px, py, cx, cy, hw - line, hh - line, radius - line), py - split_y);
        if (button == 0) d = std::max(d, px - cx + line * 0.5f);
        if (button == 1) d = std::max(d, cx - px + line * 0.5f);
        if (button == 2) d = std::max(d, std::abs(px - cx) - size * 0.07f);
        return d;
    });
    canvas.Fill(cx - hw, cy - hh, cx + hw, cy + hh, kInk, [&](float px, float py) {
        float inside = RoundedRect(px, py, cx, cy, hw - line / 2, hh - line / 2, radius);
        float rim = std::abs(inside) - line / 2;
        float seam = std::max(std::abs(px - cx) - line * 0.3f, std::max(py - split_y, inside));
        float under = std::max(std::abs(py - split_y) - line * 0.3f, inside);
        return std::min(rim, std::min(seam, under));
    });
}

void Keycap(Canvas& canvas, float left, float top, float w, float h, const std::string& label) {
    float size = std::min(w, h);
    float cx = left + w / 2, cy = top + h / 2 - size * 0.02f;
    float r = size * 0.46f, ring = std::max(1.0f, size * 0.085f), drop = size * 0.04f;
    auto disc = [&](float rr) { return [&, rr](float px, float py) { return std::hypot(px - cx, py - cy) - rr; }; };
    // the shadow, the metal ring lit from the top, the face dark below
    canvas.Fill(cx - r, cy - r + drop, cx + r, cy + r + drop, kShadow,
                [&](float px, float py) { return std::hypot(px - cx, py - cy - drop) - r; });
    Shade(canvas, cy - r, cy + r, cx - r, cx + r, kRingTop, kRingBottom, disc(r));
    float face = r - ring;
    Shade(canvas, cy - face, cy + face, cx - face, cx + face, kFaceTop, kFaceBottom, disc(face));
    // a thin dark line inside the ring, as the game's keys have
    canvas.Fill(cx - face, cy - face, cx + face, cy + face, Color{0, 0, 0, 0.7f},
                [&](float px, float py) { return std::abs(std::hypot(px - cx, py - cy) - face) - std::max(0.5f, size * 0.012f); });
    float inner = face * 2 * 0.80f;  // the text's width inside the face
    float a = face * 0.95f;          // arrow size
    if (label == "@lmb" || label == "@rmb" || label == "@mmb") {
        Mouse(canvas, cx, cy, face * 1.25f, label == "@lmb" ? 0 : label == "@rmb" ? 1 : 2);
    } else if (label == "@up" || label == "@down" || label == "@left" || label == "@right") {
        Arrow(canvas, cx, cy, a, label == "@left" ? -1 : label == "@right" ? 1 : 0,
              label == "@up" ? -1 : label == "@down" ? 1 : 0, kInk);
    } else if (label == "@leftright") {
        Arrow(canvas, cx - a * 0.42f, cy, a * 0.62f, -1, 0, kInk);
        Arrow(canvas, cx + a * 0.42f, cy, a * 0.62f, 1, 0, kInk);
    } else if (label == "@updown") {
        Arrow(canvas, cx, cy - a * 0.42f, a * 0.62f, 0, -1, kInk);
        Arrow(canvas, cx, cy + a * 0.42f, a * 0.62f, 0, 1, kInk);
    } else if (label == "@arrows") {
        float s = a * 0.45f, o = a * 0.4f;
        Arrow(canvas, cx, cy - o, s, 0, -1, kInk);
        Arrow(canvas, cx, cy + o, s, 0, 1, kInk);
        Arrow(canvas, cx - o, cy, s, -1, 0, kInk);
        Arrow(canvas, cx + o, cy, s, 1, 0, kInk);
    } else {
        // a letter as big as the typewriter's, a capital as on a keyboard; longer names as wide as fits
        std::string text = label;
        // names fill the face edge to edge, a few letters still read at icon size
        Text(canvas, cx, cy, text.size() == 1 ? inner : face * 1.92f, text.size() == 1 ? face * 0.95f : face * 0.9f,
             text, kInk);
    }
}

// a DXT3 texture's texels (straight alpha)
std::vector<Color> DecodeDxt3(const std::vector<uint8_t>& data, uint32_t width, uint32_t height) {
    std::vector<Color> out(size_t(width) * height);
    auto rgb = [](uint16_t c) {
        return Color{float((c >> 11) & 31) / 31.0f, float((c >> 5) & 63) / 63.0f, float(c & 31) / 31.0f, 1.0f};
    };
    size_t at = 0;
    for (uint32_t by = 0; by < height; by += 4)
        for (uint32_t bx = 0; bx < width; bx += 4, at += 16) {
            if (at + 16 > data.size()) return out;
            const uint8_t* b = data.data() + at;
            Color c0 = rgb(uint16_t(b[8] | b[9] << 8)), c1 = rgb(uint16_t(b[10] | b[11] << 8));
            Color pal[4] = {c0, c1,
                            {(2 * c0.r + c1.r) / 3, (2 * c0.g + c1.g) / 3, (2 * c0.b + c1.b) / 3, 1},
                            {(c0.r + 2 * c1.r) / 3, (c0.g + 2 * c1.g) / 3, (c0.b + 2 * c1.b) / 3, 1}};
            uint32_t bits = uint32_t(b[12] | b[13] << 8 | b[14] << 16 | uint32_t(b[15]) << 24);
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx + uint32_t(i & 3), y = by + uint32_t(i >> 2);
                if (x >= width || y >= height) continue;
                Color c = pal[(bits >> (2 * i)) & 3];
                c.a = float((b[i / 2] >> (4 * (i & 1))) & 15) / 15.0f;
                out[size_t(y) * width + x] = c;
            }
        }
    return out;
}

// a pad's shoulder button: a dark rounded bar lit along its top, LB or RB on it in white
void Bumper(Canvas& canvas, float left, float top, float w, float h, const std::string& name) {
    float size = std::min(w, h);
    float cx = left + w / 2, cy = top + h / 2;
    float hw = std::min(w * 0.47f, size * 0.62f), hh = size * 0.30f, radius = hh * 0.9f;
    auto bar = [&](float grow) {
        return [&, grow](float px, float py) { return RoundedRect(px, py, cx, cy, hw + grow, hh + grow, radius + grow); };
    };
    float rim = std::max(1.0f, size * 0.05f), drop = size * 0.04f;
    canvas.Fill(cx - hw, cy - hh + drop, cx + hw, cy + hh + drop, kShadow, [&](float px, float py) {
        return RoundedRect(px, py, cx, cy + drop, hw, hh, radius);
    });
    Shade(canvas, cy - hh, cy + hh, cx - hw, cx + hw, Color{0.78f, 0.78f, 0.80f, 1}, Color{0.32f, 0.32f, 0.35f, 1},
          bar(0));
    Shade(canvas, cy - hh + rim, cy + hh - rim, cx - hw + rim, cx + hw - rim, Color{0.22f, 0.22f, 0.24f, 1},
          Color{0.08f, 0.08f, 0.09f, 1}, bar(-rim));
    Text(canvas, cx, cy, (hw - rim) * 1.7f, (hh - rim) * 1.5f, name, Color{0.97f, 0.97f, 0.97f, 1});
}

std::shared_ptr<const Picture> Draw(const IconTexture& t, const Labels& labels, int scale) {
    Canvas canvas(t.width * uint32_t(scale), t.height * uint32_t(scale));
    float s = float(scale);
    if (labels[kA] == kControllerSet) {
        // the game's own pictures, bilinear to the new size, then the shoulder buttons over the white and black
        std::vector<Color> texels = DecodeDxt3(t.level0, t.width, t.height);
        auto at = [&](int x, int y) {
            x = std::clamp(x, 0, int(t.width) - 1), y = std::clamp(y, 0, int(t.height) - 1);
            return texels[size_t(y) * t.width + size_t(x)];
        };
        for (uint32_t y = 0; y < t.height * uint32_t(scale); y++)
            for (uint32_t x = 0; x < t.width * uint32_t(scale); x++) {
                float fx = (float(x) + 0.5f) / s - 0.5f, fy = (float(y) + 0.5f) / s - 0.5f;
                int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
                float ax = fx - float(x0), ay = fy - float(y0);
                Color c{0, 0, 0, 0};
                float wsum = 0;
                for (int k = 0; k < 4; k++) {
                    Color p = at(x0 + (k & 1), y0 + (k >> 1));
                    float wgt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay) * p.a;  // alpha weighted
                    c.r += p.r * wgt, c.g += p.g * wgt, c.b += p.b * wgt, wsum += wgt;
                    c.a += p.a * ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                }
                if (wsum > 0) c.r /= wsum, c.g /= wsum, c.b /= wsum;
                canvas.Set(int(x), int(y), c);
            }
        for (const Box& b : t.boxes) {
            const std::string& l = labels[b.icon];
            if (l != "@lb" && l != "@rb") continue;
            for (int y = int(float(b.y) * s); y < int(float(b.y + b.height) * s); y++)
                for (int x = int(float(b.x) * s); x < int(float(b.x + b.width) * s); x++) canvas.Set(x, y, Color{1, 1, 1, 0});
            Bumper(canvas, float(b.x) * s, float(b.y) * s, float(b.width) * s, float(b.height) * s,
                   l == "@lb" ? "LB" : "RB");
        }
        return canvas.Finish();
    }
    for (const Box& b : t.boxes)
        Keycap(canvas, float(b.x) * s, float(b.y) * s, float(b.width) * s, float(b.height) * s, labels[b.icon]);
    return canvas.Finish();
}

// the main menu's paper without its fourth box (rows 346-363, columns 28-48): the paper 24 texels right of it
std::shared_ptr<const Picture> MenuPaper() {
    constexpr uint32_t w = 256, h = 512;
    std::vector<Color> texels = DecodeDxt3(g_menu_paper, w, h);
    for (uint32_t y = 342; y < 368; y++)
        for (uint32_t x = 24; x < 52; x++) texels[y * w + x] = texels[y * w + x + 26];
    auto pic = std::make_shared<Picture>();
    pic->width = w;
    pic->height = h;
    pic->argb.resize(texels.size());
    auto channel = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (size_t i = 0; i < texels.size(); i++) {
        const Color& c = texels[i];
        pic->argb[i] = channel(c.a) << 24 | channel(c.r) << 16 | channel(c.g) << 8 | channel(c.b);
    }
    return pic;
}

// render thread: keycaps for the icon textures while labels are set
std::shared_ptr<const Picture> Provide(const gpu::TextureDesc& d, const uint8_t* guest, float render_scale) {
    std::call_once(g_loaded, Load);
    const IconTexture* found = nullptr;
    for (const IconTexture& t : g_textures)
        if (t.width == d.width && t.height == d.height && gpu::LevelSize(d, 0) == t.level0.size() &&
            std::memcmp(guest, t.level0.data(), t.level0.size()) == 0)
            found = &t;
    if (!found) {
        if (g_menu_paper.empty() || d.width != 256 || d.height != 512 || gpu::LevelSize(d, 0) != g_menu_paper.size() ||
            std::memcmp(guest, g_menu_paper.data(), g_menu_paper.size()) != 0)
            return nullptr;
        std::lock_guard lock(g_lock);
        if (!g_menu_paper_picture) g_menu_paper_picture = MenuPaper();
        return g_menu_paper_picture;
    }
    std::lock_guard lock(g_lock);
    if (!g_labels) return nullptr;
    // a controller's pictures change only in the icon font (DXT3), where the white and black buttons are
    if ((*g_labels)[kA] == kControllerSet && (found->level0.size() != size_t(found->width) * found->height ||
                                              std::none_of(found->boxes.begin(), found->boxes.end(), [](const Box& b) {
                                                  return b.icon == kWhite || b.icon == kBlack;
                                              })))
        return nullptr;
    // drawn at the output's resolution: at least 2, at most 8 times the original
    int scale = std::clamp(int(std::ceil(render_scale - 0.01f)), 2, 8);
    auto& cached = g_cache[{found, scale}];
    if (!cached) cached = Draw(*found, *g_labels, scale);
    return cached;
}

void SetLabels(const std::optional<Labels>& labels) {
    {
        std::lock_guard lock(g_lock);
        if (labels == g_labels) return;
        auto pad = [](const std::optional<Labels>& l) { return !l || (*l)[kA] == kControllerSet; };
        if (!g_labels || pad(labels) != pad(g_labels))
            LOGR("button prompts: %s", pad(labels) ? "controller" : "keyboard and mouse");
        g_labels = labels;
        g_cache.clear();
    }
    gpu::texture_override::Changed();
}

// a key's name in the settings (mouse_keyboard.cpp) as a keycap shows it
std::string CapLabel(const std::string& key) {
    static constexpr std::pair<const char*, const char*> kShort[] = {
        {"LMB", "@lmb"}, {"RMB", "@rmb"}, {"MMB", "@mmb"}, {"Up", "@up"}, {"Down", "@down"}, {"Left", "@left"},
        {"Right", "@right"}, {"Escape", "Esc"}, {"Backspace", "Bksp"}, {"CapsLock", "Caps"}, {"Wheel Up", "Wheel"},
        {"Wheel Down", "Wheel"}, {"Mouse4", "M4"}, {"Mouse5", "M5"}, {"PageUp", "PgUp"}, {"PageDown", "PgDn"},
        {"Delete", "Del"}, {"Insert", "Ins"}, {"Backtick", "`"}, {"Minus", "-"}, {"Equals", "="},
        {"LBracket", "["}, {"RBracket", "]"}, {"Backslash", "\\"}, {"Semicolon", ";"}, {"Quote", "'"},
        {"Comma", ","}, {"Period", "."}, {"Slash", "/"}, {"NumpadPlus", "Num+"}, {"NumpadMinus", "Num-"},
        {"NumpadStar", "Num*"}, {"NumpadSlash", "Num/"}, {"NumpadPeriod", "Num."}};
    for (const auto& [name, label] : kShort)
        if (key == name) return label;
    if (key.rfind("Numpad", 0) == 0) return "Num" + key.substr(6);
    return key;
}

// the action each default control (kPadControls' order in mouse_look.cpp) comes from
constexpr const char* kControlActions[] = {"use",   "crouch",      "reload",    "jump",      "melee",
                                           "grenade", "aim",       "fire",      "right_stick", "sprint",
                                           "next_weapon", "dpad_down", "lean_left", "lean_right"};
constexpr Icon kControlIcons[] = {kA,         kB,        kX,          kY,       kBlack,     kWhite,     kLeftTrigger,
                                  kRightTrigger, kIconCount, kIconCount, kDpadUp, kDpadDown, kDpadLeft, kDpadRight};

void Update() {
    uint32_t mode = game::Opt().button_prompts;
    bool keyboard = mode == 1 || (mode != 2 && input::KeyboardActive());
    if (!keyboard) {
        // a pad's LB and RB for the original's white and black buttons
        Labels pad;
        pad[kA] = kControllerSet;
        pad[kWhite] = "@lb";
        pad[kBlack] = "@rb";
        SetLabels(pad);
        return;
    }
    Labels labels;
    for (size_t i = 0; i < std::size(kControlActions); i++) {
        std::string key = input::KeyName(kControlActions[i]);
        int to = game::PadTarget(i);
        if (key.empty() || to < 0) continue;
        Icon icon = kControlIcons[to];
        if (icon != kIconCount && labels[icon].empty()) labels[icon] = CapLabel(key);
    }
    // a control that runs its command itself is on no button: the lean keys are the dpad's left and right, and use's
    // key the X of "Press X to pick up" where the layout joins use and reload
    auto own = [&](Icon icon, const char* action) {
        std::string key = input::KeyName(action);
        if (labels[icon].empty() && !key.empty()) labels[icon] = CapLabel(key);
    };
    own(kDpadLeft, "lean_left");
    own(kDpadRight, "lean_right");
    own(kX, "use");
    // crouch is a command now, on no button: the hints' B ("Press B to crouch") is its key
    if (labels[kB].empty()) {
        std::string crouch = input::KeyName("crouch");
        if (!crouch.empty()) labels[kB] = CapLabel(crouch);
    }
    // in menus the arrows are the dpad, Enter A and Backspace B
    bool menu = !input::Playing();
    if (menu) {
        labels[kDpadUp] = "@up";
        labels[kDpadDown] = "@down";
        labels[kDpadLeft] = "@left";
        labels[kDpadRight] = "@right";
        labels[kA] = "Enter";
        labels[kB] = input::TextEntry() ? "Esc" : "Bksp";  // a name entry screen's Backspace deletes
    }
    bool arrows = labels[kDpadLeft] == "@left" && labels[kDpadRight] == "@right";
    labels[kDpadLeftRight] = menu || arrows || labels[kDpadLeft].empty() || labels[kDpadRight].empty()
                                 ? "@leftright"
                                 : labels[kDpadLeft] + "/" + labels[kDpadRight];
    labels[kDpadUpDown] = "@updown";
    labels[kDpad] = "@arrows";
    std::string pause = input::KeyName("pause"), objectives = input::KeyName("objectives");
    labels[kStart] = pause.empty() ? "Esc" : CapLabel(pause);
    labels[kBack] = objectives.empty() ? "Tab" : CapLabel(objectives);
    SetLabels(labels);
}

}  // namespace

namespace game {

void InitPrompts() { gpu::texture_override::SetProvider(Provide); }

// a game file, also before boot
bool ReadEarlyGameFile(const std::string& xbox_path, std::vector<uint8_t>& out) { return ReadAny(xbox_path, 0, 0, out); }

// a DXT3 picture of the front end's pack (sp/frontend.cod) as straight RGBA bytes, for the PC Options page
bool ReadFrontEndPicture(const std::string& name, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    constexpr const char* kFrontEnd = "D:\\sp\\frontend.cod";
    std::map<std::string, Entry> files = ReadDirectory(kFrontEnd);
    auto it = files.find(name);
    std::vector<uint8_t> dds;
    if (it == files.end() || !ReadAny(kFrontEnd, it->second.offset, it->second.size, dds)) return false;
    if (dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || std::memcmp(dds.data() + 84, "DXT3", 4) != 0)
        return false;
    height = U32(dds, 12);
    width = U32(dds, 16);
    uint32_t size = Level0Size(dds, width, height);
    if (!width || !height || width > 1024 || height > 1024 || !size || 128 + size > dds.size()) return false;
    std::vector<Color> texels = DecodeDxt3(std::vector<uint8_t>(dds.begin() + 128, dds.begin() + 128 + size), width,
                                           height);
    rgba.resize(texels.size() * 4);
    auto byte = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t i = 0; i < texels.size(); i++) {
        rgba[i * 4] = byte(texels[i].r);
        rgba[i * 4 + 1] = byte(texels[i].g);
        rgba[i * 4 + 2] = byte(texels[i].b);
        rgba[i * 4 + 3] = byte(texels[i].a);
    }
    return true;
}

// each client frame, in menus and in play
void UpdatePrompts() { Update(); }

}  // namespace game
