// Keyboard and mouse button prompts. The game shows its controller buttons as pictures: an icon font
// (i_button_icons_xb_<language>.dds with its .fdf table, codes 33-49: triggers, A B X Y, white and black, the dpad,
// start and back) and four footer icons (Xbox_A/B/X/YButton.dds), all in sp/global.cod. While the keyboard and mouse
// are in use the renderer draws keycaps and mouse pictures in their place (gpu/texture_override.h), labelled with the
// keys that give each button in the controller layout picked in the game's options; the controller's icons come back
// the moment a controller is used. After CoD2-BRO-Native-Loader's text/key_prompts.cpp (GPL-2.0-or-later,
// THIRD_PARTY_NOTICES.md).

#include <algorithm>
#include <array>
#include <atomic>
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

std::map<std::string, Entry> ReadDirectory() {
    std::map<std::string, Entry> files;
    std::vector<uint8_t> h;
    if (!kernel::ReadGameFile(kPack, 0, 0x8000, h) || std::memcmp(h.data(), "KAPF", 4) != 0) return files;
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
    std::vector<std::string> candidates;
#ifdef _WIN32
    const char* windir = std::getenv("WINDIR");
    std::string fonts = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    for (const char* f : {"segoeuib.ttf", "arialbd.ttf", "tahomabd.ttf", "arial.ttf"}) candidates.push_back(fonts + f);
#else
    candidates = {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
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

constexpr Color kInk{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color kOutline{0.86f, 0.86f, 0.86f, 1.0f};
constexpr Color kCapTop{0.24f, 0.24f, 0.26f, 1.0f};
constexpr Color kCapBottom{0.10f, 0.10f, 0.11f, 1.0f};
constexpr Color kLit{0.95f, 0.75f, 0.25f, 1.0f};

// a mouse with one button lit: 0 left, 1 right, 2 middle
void Mouse(Canvas& canvas, float left, float top, float w, float h, int button) {
    float size = std::min(w, h);
    float cx = left + w / 2, cy = top + h / 2;
    float line = std::max(1.0f, size * 0.07f);
    float hw = size * 0.31f, hh = size * 0.46f, radius = hw * 0.95f;
    canvas.Fill(cx - hw, cy - hh, cx + hw, cy + hh, kCapBottom,
                [&](float px, float py) { return RoundedRect(px, py, cx, cy, hw, hh, radius); });
    float split_y = cy - hh * 0.15f;  // where the buttons end
    canvas.Fill(cx - hw, cy - hh, cx + hw, split_y, kLit, [&](float px, float py) {
        float d = std::max(RoundedRect(px, py, cx, cy, hw - line, hh - line, radius - line), py - split_y);
        if (button == 0) d = std::max(d, px - cx);
        if (button == 1) d = std::max(d, cx - px);
        if (button == 2) d = std::max(d, std::abs(px - cx) - size * 0.06f);
        return d;
    });
    // the outline, the line between the buttons and the one under them
    canvas.Fill(cx - hw, cy - hh, cx + hw, cy + hh, kOutline, [&](float px, float py) {
        float inside = RoundedRect(px, py, cx, cy, hw - line / 2, hh - line / 2, radius);
        float body = std::abs(inside) - line / 2;
        float middle = std::max(std::abs(px - cx) - line * 0.35f, std::max(py - split_y, inside));
        float under = std::max(std::abs(py - split_y) - line * 0.35f, inside);
        return std::min(body, std::min(middle, under));
    });
}

void Keycap(Canvas& canvas, float left, float top, float w, float h, const std::string& label) {
    if (label == "@lmb" || label == "@rmb" || label == "@mmb") {
        Mouse(canvas, left, top, w, h, label == "@lmb" ? 0 : label == "@rmb" ? 1 : 2);
        return;
    }
    float size = std::min(w, h);
    float margin = size * 0.05f, line = std::max(1.0f, size * 0.06f);
    float cx = left + w / 2, cy = top + h / 2, hw = w / 2 - margin, hh = h / 2 - margin;
    float radius = size * 0.2f;
    for (int y = int(cy - hh) - 1; y <= int(cy + hh) + 1; y++) {  // the cap, lit from the top
        float t = std::clamp((float(y) + 0.5f - (cy - hh)) / (2 * hh), 0.0f, 1.0f);
        Color c{kCapTop.r + (kCapBottom.r - kCapTop.r) * t, kCapTop.g + (kCapBottom.g - kCapTop.g) * t,
                kCapTop.b + (kCapBottom.b - kCapTop.b) * t, 1.0f};
        canvas.Fill(cx - hw, float(y), cx + hw, float(y), c, [&](float px, float py) {
            return std::max(RoundedRect(px, py, cx, cy, hw, hh, radius), std::abs(py - (float(y) + 0.5f)) - 0.5f);
        });
    }
    canvas.Fill(cx - hw, cy - hh, cx + hw, cy + hh, kOutline, [&](float px, float py) {
        return std::abs(RoundedRect(px, py, cx, cy, hw - line / 2, hh - line / 2, radius - line / 2)) - line / 2;
    });
    float inner_w = 2 * (hw - line) * 0.88f, inner_h = 2 * (hh - line);
    float a = size * 0.42f;  // arrow size
    if (label == "@up" || label == "@down" || label == "@left" || label == "@right") {
        Arrow(canvas, cx, cy, a, label == "@left" ? -1 : label == "@right" ? 1 : 0,
              label == "@up" ? -1 : label == "@down" ? 1 : 0, kInk);
    } else if (label == "@leftright") {
        Arrow(canvas, cx - a * 0.45f, cy, a * 0.7f, -1, 0, kInk);
        Arrow(canvas, cx + a * 0.45f, cy, a * 0.7f, 1, 0, kInk);
    } else if (label == "@updown") {
        Arrow(canvas, cx, cy - a * 0.45f, a * 0.7f, 0, -1, kInk);
        Arrow(canvas, cx, cy + a * 0.45f, a * 0.7f, 0, 1, kInk);
    } else if (label == "@arrows") {
        float s = a * 0.5f, o = a * 0.42f;
        Arrow(canvas, cx, cy - o, s, 0, -1, kInk);
        Arrow(canvas, cx, cy + o, s, 0, 1, kInk);
        Arrow(canvas, cx - o, cy, s, -1, 0, kInk);
        Arrow(canvas, cx + o, cy, s, 1, 0, kInk);
    } else {
        // one letter at about half the cap's height, longer names as wide as fits
        Text(canvas, cx, cy, inner_w, label.size() == 1 ? inner_h * 0.5f : inner_h * 0.42f, label, kInk);
    }
}

std::shared_ptr<const Picture> Draw(const IconTexture& t, const Labels& labels, int scale) {
    Canvas canvas(t.width * uint32_t(scale), t.height * uint32_t(scale));
    float s = float(scale);
    for (const Box& b : t.boxes)
        Keycap(canvas, float(b.x) * s, float(b.y) * s, float(b.width) * s, float(b.height) * s, labels[b.icon]);
    return canvas.Finish();
}

// render thread: keycaps for the icon textures while labels are set
std::shared_ptr<const Picture> Provide(const gpu::TextureDesc& d, const uint8_t* guest, float render_scale) {
    std::call_once(g_loaded, Load);
    const IconTexture* found = nullptr;
    for (const IconTexture& t : g_textures)
        if (t.width == d.width && t.height == d.height && gpu::LevelSize(d, 0) == t.level0.size() &&
            std::memcmp(guest, t.level0.data(), t.level0.size()) == 0)
            found = &t;
    if (!found) return nullptr;
    std::lock_guard lock(g_lock);
    if (!g_labels) return nullptr;
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
        if (labels.has_value() != g_labels.has_value())
            LOGR("button prompts: %s", labels ? "keyboard and mouse" : "controller");
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
        SetLabels(std::nullopt);
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

// each client frame, in menus and in play
void UpdatePrompts() { Update(); }

}  // namespace game
