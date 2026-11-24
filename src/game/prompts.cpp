// Keyboard and mouse button prompts. The game shows its controller buttons as pictures: an icon font
// (i_button_icons_xb_<language>.dds with its .fdf table, codes 33-49: triggers, A B X Y, white and black, the dpad,
// start and back) and four footer icons (Xbox_A/B/X/YButton.dds), all in sp/global.cod. While the keyboard and mouse
// are in use the renderer draws keycaps and mouse pictures in their place (gpu/texture_override.h), labelled with the
// keys that give each button in the controller layout picked in the game's options; the controller's icons come back
// the moment a controller is used; with the Modern layout picked a controller's buttons are pictures of an
// Xbox 360 controller's instead (button_images.cpp), the other layouts keep the game's own, white and black buttons too. After
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
#include "common/menu_state.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/texture.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"
#include "app/app.h"
#include "kernel/disc.h"

namespace game {
int PadTarget(size_t i);  // src/game/mouse_look.cpp
bool PadSameBinding(size_t a, size_t b);
void HintKeys(const char* (&actions)[2]);  // src/game/hints.cpp
std::vector<uint8_t> BoldGaramond();       // src/game/sharp_text.cpp: the menus' bold Garamond, empty for none
struct ButtonPicture {  // src/game/button_images.cpp
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> argb;
};
enum ButtonImage { kXnaA, kXnaB, kXnaX, kXnaY, kXnaBack, kXnaStart, kXnaDpad, kXnaLB, kXnaRB, kXnaLT, kXnaRT,
                   kPsCross, kPsCircle, kPsSquare, kPsTriangle, kPsL1, kPsR1, kPsL2, kPsR2, kPsDpad,
                   kPsDpadUp, kPsDpadDown, kPsDpadLeft, kPsDpadRight, kPsDpadLeftRight, kPsDpadUpDown,
                   kPsOptions, kPsShare, kXnaCount };
const ButtonPicture* XnaButton(int which);
// src/game/sharp_text.cpp: a game font sheet drawn again sharp, null for any other texture
std::shared_ptr<const gpu::texture_override::Picture> SmoothSky(const gpu::TextureDesc& d, const uint8_t* guest);
std::shared_ptr<const gpu::texture_override::Picture> SharpText(const gpu::TextureDesc& d, const uint8_t* guest,
                                                                float render_scale);
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
// labels[kA] saying this: a controller is in use, the game's own pictures stay
constexpr const char* kControllerSet = "@controller";
// labels[kStart] saying this with kControllerSet: the Modern layout's controller, every icon a picture from button_images.cpp
constexpr const char* kXnaSet = "@xna";
// the same with a PlayStation pad in use: its buttons' pictures
constexpr const char* kPsSet = "@ps";

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
// the Controller screen's picture (xboxlineA.dds in sp/frontend.cod): the Modern one in its place (pad_picture.cpp)
std::vector<uint8_t> g_pad_picture;  // its level 0, written once under g_loaded
// the crosshair's three pictures (center_dot 8 x 8, side_skinny 8 x 8, side_small_arc 16 x 16, the same in every
// mission's weapons.cod): a few texels each, drawn again smooth at the output's size. Level 0 of each, written once
// under g_loaded
constexpr const char* kReticleNames[] = {"center_dot.dds", "side_skinny.dds", "side_small_arc.dds"};
constexpr uint32_t kReticleSizes[] = {8, 8, 16};
std::vector<uint8_t> g_reticle[3];
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
    // the bold Garamond of the settings menu's keys, else the game's own regular one
    g_ttf = game::BoldGaramond();
    if (!g_ttf.empty() && stbtt_InitFont(&g_font, g_ttf.data(), stbtt_GetFontOffsetForIndex(g_ttf.data(), 0))) return true;
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
    if (auto it = front.find("xboxlineA.dds"); it != front.end() &&
        kernel::ReadGameFile("D:\\sp\\frontend.cod", it->second.offset, it->second.size, dds) &&
        dds.size() >= 128 + 512 * 512 && U32(dds, 16) == 512 && U32(dds, 12) == 512)
        g_pad_picture.assign(dds.begin() + 128, dds.begin() + 128 + 512 * 512);
    const char* weapons = "D:\\sp\\prologue\\weapons.cod";
    std::map<std::string, Entry> pack = ReadDirectory(weapons);
    int reticles = 0;
    for (int i = 0; i < 3; i++) {
        auto it = pack.find(kReticleNames[i]);
        if (it == pack.end() || !kernel::ReadGameFile(weapons, it->second.offset, it->second.size, dds)) continue;
        if (dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || U32(dds, 16) != kReticleSizes[i] ||
            U32(dds, 12) != kReticleSizes[i])
            continue;
        uint32_t size = Level0Size(dds, kReticleSizes[i], kReticleSizes[i]);
        if (!size || 128 + size > dds.size()) continue;
        g_reticle[i].assign(dds.begin() + 128, dds.begin() + 128 + size);
        reticles++;
    }
    LOGR("crosshair: %d of its 3 pictures found to draw smooth", reticles);
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

// The settings menu's own key (Cap in common/modern_menu.cpp), the same one: a flat cream cap, its corners rounded by
// 0.14 of its height, the key's name on it in dark ink in the bold Garamond at 0.62 of its height. No edge, no shadow.
constexpr Color kInk{0.14f, 0.11f, 0.08f, 1.0f};
constexpr Color kCream{0.92f, 0.88f, 0.78f, 1.0f};
constexpr Color kLit{0.66f, 0.13f, 0.10f, 1.0f};

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
    bool name = label.size() > 1 && label[0] != '@';  // a name's cap is as wide as its place, a letter's square
    float cx = left + w / 2, cy = top + h / 2 - size * 0.02f;
    float hh = size * 0.42f, hw = name ? std::max(hh, w * 0.5f - std::max(0.5f, size * 0.02f)) : hh;
    float radius = hh * 0.28f;  // 0.14 of the cap's height
    canvas.Fill(cx - hw, cy - hh, cx + hw, cy + hh, kCream,
                [&](float px, float py) { return RoundedRect(px, py, cx, cy, hw, hh, radius); });
    const float edge = 0.0f;
    float face = hh;
    float inner = face * 2 * 0.80f;  // the text's width inside the face
    float a = face * 0.8f;           // arrow size
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
        // at the menu's size (a capital 0.4 of the cap high); a name shrinks to fit its cap
        Text(canvas, cx, cy, name ? (hw - edge) * 2 * 0.86f : inner, face * 0.8f, label, kInk);
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

// a picture fitted into a box (its shape kept, centred), each pixel the average of the picture's under it
void FitPicture(Canvas& canvas, const game::ButtonPicture& p, float left, float top, float w, float h) {
    float fit = std::min(w / float(p.width), h / float(p.height));
    float dw = float(p.width) * fit, dh = float(p.height) * fit;
    float x0 = left + (w - dw) / 2, y0 = top + (h - dh) / 2;
    // the source pixels under each output column (row) and how much of each
    auto spans = [](float start, float length, uint32_t source, int& first, int& count) {
        first = int(std::floor(start));
        count = int(std::ceil(start + length)) - first;
        std::vector<std::vector<std::pair<int, float>>> out(size_t(std::max(0, count)));
        float per = float(source) / length;
        for (int i = 0; i < count; i++) {
            float a = std::max(float(first + i), start), b = std::min(float(first + i + 1), start + length);
            if (b <= a) continue;
            float s0 = (a - start) * per, s1 = (b - start) * per;
            for (int k = int(std::floor(s0)); k < int(std::ceil(s1)) && k < int(source); k++) {
                float o = std::min(s1, float(k + 1)) - std::max(s0, float(k));
                if (o > 0 && k >= 0) out[size_t(i)].emplace_back(k, o / per);
            }
        }
        return out;
    };
    int fx, nx, fy, ny;
    auto cols = spans(x0, dw, p.width, fx, nx);
    auto rows = spans(y0, dh, p.height, fy, ny);
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            float r = 0, g = 0, b = 0, a = 0, area = 0;
            for (auto [sy, wy] : rows[size_t(j)])
                for (auto [sx, wx] : cols[size_t(i)]) {
                    uint32_t c = p.argb[size_t(sy) * p.width + size_t(sx)];
                    float wgt = wx * wy, al = float(c >> 24) / 255.0f * wgt;  // premultiplied
                    r += float((c >> 16) & 255) / 255.0f * al, g += float((c >> 8) & 255) / 255.0f * al;
                    b += float(c & 255) / 255.0f * al, a += al, area += wgt;
                }
            if (a <= 0) continue;
            canvas.Blend(fx + i, fy + j, Color{r / a, g / a, b / a, 1.0f}, a);  // a: covered part of the pixel
        }
}

// the Modern layout's controller: each icon the picture of its button (button_images.cpp); the dpad's directions are the dpad with
// arrows on the arms pressed. With a PlayStation pad its own buttons, its dpad's arms lit, and Options and Share in
// Start's and Back's places
void DrawXna(Canvas& canvas, const IconTexture& t, float s, bool playstation) {
    for (const Box& b : t.boxes) {
        int which = -1;
        switch (b.icon) {
        case kA: which = playstation ? game::kPsCross : game::kXnaA; break;
        case kB: which = playstation ? game::kPsCircle : game::kXnaB; break;
        case kX: which = playstation ? game::kPsSquare : game::kXnaX; break;
        case kY: which = playstation ? game::kPsTriangle : game::kXnaY; break;
        case kRightTrigger: which = playstation ? game::kPsR2 : game::kXnaRT; break;
        case kLeftTrigger: which = playstation ? game::kPsL2 : game::kXnaLT; break;
        // the layout's LB and RB are the original's white and black
        case kWhite: which = playstation ? game::kPsL1 : game::kXnaLB; break;
        case kBlack: which = playstation ? game::kPsR1 : game::kXnaRB; break;
        case kStart: which = playstation ? game::kPsOptions : game::kXnaStart; break;  // Options and Share in their places
        case kBack: which = playstation ? game::kPsShare : game::kXnaBack; break;
        case kDpadUp: which = playstation ? game::kPsDpadUp : game::kXnaDpad; break;
        case kDpadDown: which = playstation ? game::kPsDpadDown : game::kXnaDpad; break;
        case kDpadLeft: which = playstation ? game::kPsDpadLeft : game::kXnaDpad; break;
        case kDpadRight: which = playstation ? game::kPsDpadRight : game::kXnaDpad; break;
        case kDpadLeftRight: which = playstation ? game::kPsDpadLeftRight : game::kXnaDpad; break;
        case kDpadUpDown: which = playstation ? game::kPsDpadUpDown : game::kXnaDpad; break;
        default: which = playstation ? game::kPsDpad : game::kXnaDpad; break;
        }
        bool dpad = which == game::kXnaDpad;  // the Xbox pad's has no pictures of its arms pressed: arrows on it
        float left = float(b.x) * s, top = float(b.y) * s, w = float(b.width) * s, h = float(b.height) * s;
        const game::ButtonPicture* p = game::XnaButton(which);
        if (!p) continue;
        // a small margin, none for the wide shoulder buttons, which are short enough already
        float pad = which == game::kXnaLB || which == game::kXnaRB ? 0.0f : std::max(0.5f, w * 0.02f);
        FitPicture(canvas, *p, left + pad, top + pad, w - 2 * pad, h - 2 * pad);
        if (!dpad) continue;
        float cx = left + w / 2, cy = top + h / 2, reach = std::min(w, h) * 0.27f, size = std::min(w, h) * 0.24f;
        const Color lit{0.97f, 0.97f, 0.97f, 1.0f}, rim{0.0f, 0.0f, 0.0f, 0.6f};
        auto arm = [&](int dx, int dy) {
            Arrow(canvas, cx + float(dx) * reach, cy + float(dy) * reach, size * 1.3f, dx, dy, rim);
            Arrow(canvas, cx + float(dx) * reach, cy + float(dy) * reach, size, dx, dy, lit);
        };
        switch (b.icon) {
        case kDpadUp: arm(0, -1); break;
        case kDpadDown: arm(0, 1); break;
        case kDpadLeft: arm(-1, 0); break;
        case kDpadRight: arm(1, 0); break;
        case kDpadLeftRight: arm(-1, 0), arm(1, 0); break;
        case kDpadUpDown: arm(0, -1), arm(0, 1); break;
        default: break;  // the whole dpad
        }
    }
}

std::shared_ptr<const Picture> Draw(const IconTexture& t, const Labels& labels, int scale) {
    Canvas canvas(t.width * uint32_t(scale), t.height * uint32_t(scale));
    float s = float(scale);
    if (labels[kA] == kControllerSet && (labels[kStart] == kXnaSet || labels[kStart] == kPsSet)) {
        DrawXna(canvas, t, s, labels[kStart] == kPsSet);
        return canvas.Finish();
    }
    for (const Box& b : t.boxes)
        Keycap(canvas, float(b.x) * s, float(b.y) * s, float(b.width) * s, float(b.height) * s, labels[b.icon]);
    return canvas.Finish();
}

// The crosshair's pictures drawn again, scale times their size, where the game's texels are: the dot a disc of 3
// texels about (4, 4); the thin mark a bar from (3, 3) to (4, 8); the side mark an arc of a circle of 9.7 texels about
// (8, 12), 42 degrees each way from straight up, 1.5 thick and as faint as the game's (0.42)
std::shared_ptr<const Picture> DrawReticle(int which, int scale) {
    float s = float(scale);
    Canvas canvas(kReticleSizes[which] * uint32_t(scale), kReticleSizes[which] * uint32_t(scale));
    const Color white{1.0f, 0.985f, 1.0f, 1.0f};
    float size = float(kReticleSizes[which]) * s;
    if (which == 0) {
        canvas.Fill(0, 0, size, size, white, [&](float px, float py) { return std::hypot(px - 4.0f * s, py - 4.0f * s) - 3.0f * s; });
    } else if (which == 1) {
        canvas.Fill(0, 0, size, size, white, [&](float px, float py) {
            return std::max(std::abs(px - 3.5f * s) - 0.5f * s, std::abs(py - 5.5f * s) - 2.5f * s);
        });
    } else {
        const float cx = 8.0f * s, cy = 12.0f * s, radius = 9.7f * s, half = 0.75f * s, reach = 0.733f;  // 42 degrees
        Color faint = white;
        faint.a = 0.42f;
        canvas.Fill(0, 0, size, size, faint, [&](float px, float py) {
            float angle = std::atan2(px - cx, cy - py);  // 0 straight up
            if (std::abs(angle) <= reach) return std::abs(std::hypot(px - cx, py - cy) - radius) - half;
            float side = angle < 0 ? -1.0f : 1.0f;
            float ex = cx + side * std::sin(reach) * radius, ey = cy - std::cos(reach) * radius;
            return std::hypot(px - ex, py - ey) - half;  // a round end
        });
    }
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
    if (!found && !g_pad_picture.empty() && d.width == 512 && d.height == 512 &&
        gpu::LevelSize(d, 0) == g_pad_picture.size() && std::memcmp(guest, g_pad_picture.data(), g_pad_picture.size()) == 0)
        if (auto modern = game::ModernPadPicture()) return modern;
    if (!found && d.width == d.height && (d.width == 8 || d.width == 16)) {
        for (int i = 0; i < 3; i++) {
            if (g_reticle[i].empty() || d.width != kReticleSizes[i] || gpu::LevelSize(d, 0) != g_reticle[i].size() ||
                std::memcmp(guest, g_reticle[i].data(), g_reticle[i].size()) != 0)
                continue;
            // (a texel of these is about a pixel of the game's 480 lines)
            int scale = std::clamp(int(std::ceil(render_scale - 0.01f)), 2, 8);
            static std::map<std::pair<int, int>, std::shared_ptr<const Picture>> drawn;
            std::lock_guard lock(g_lock);
            auto& cached = drawn[{i, scale}];
            if (!cached) cached = DrawReticle(i, scale);
            return cached;
        }
    }
    if (!found)
        if (auto sky = game::SmoothSky(d, guest)) return sky;
    if (!found) return game::SharpText(d, guest, render_scale);
    std::lock_guard lock(g_lock);
    if (!g_labels) return nullptr;
    // a controller's pictures are the game's own but with the Modern layout
    if ((*g_labels)[kA] == kControllerSet && (*g_labels)[kStart] != kXnaSet && (*g_labels)[kStart] != kPsSet)
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
        Labels pad;
        pad[kA] = kControllerSet;
        // the Modern layout's: XNA Game Studio's and Xelu's pictures, or with a PlayStation pad its own buttons
        if (game::Opt().modern_layout && game::XnaButton(game::kXnaA))
            pad[kStart] = input::PlayStationPadInUse() && game::XnaButton(game::kPsCross) ? kPsSet : kXnaSet;
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
    // two buttons with one command (the Modern layout's LB and RB both throw a grenade): the key on both, whichever
    // of them a hint draws
    for (size_t i = 0; i < std::size(kControlIcons); i++)
        for (size_t j = 0; j < std::size(kControlIcons); j++) {
            Icon from = kControlIcons[i], to = kControlIcons[j];
            if (from == kIconCount || to == kIconCount || labels[from].empty() || !labels[to].empty()) continue;
            if (game::PadSameBinding(i, j)) labels[to] = labels[from];
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
    // in menus the arrows are the dpad, Enter A, Backspace B and Delete X (the profile screen's delete)
    bool menu = !input::Playing();
    if (menu) {
        labels[kDpadUp] = "@up";
        labels[kDpadDown] = "@down";
        labels[kDpadLeft] = "@left";
        labels[kDpadRight] = "@right";
        labels[kA] = "Enter";
        labels[kB] = "Esc";  // Escape and Backspace both back out; a name entry screen's Backspace deletes
        labels[kX] = "Del";
    }
    bool arrows = labels[kDpadLeft] == "@left" && labels[kDpadRight] == "@right";
    labels[kDpadLeftRight] = menu || arrows || labels[kDpadLeft].empty() || labels[kDpadRight].empty()
                                 ? "@leftright"
                                 : labels[kDpadLeft] + "/" + labels[kDpadRight];
    labels[kDpadUpDown] = "@updown";
    labels[kDpad] = "@arrows";
    // our hint line's keys (hints.cpp): on the three icons its tokens name, while it is on the screen in play
    if (!menu) {
        const char* actions[2];
        game::HintKeys(actions);
        constexpr Icon kHintIcons[] = {kDpadUpDown, kDpadLeftRight};  // hints.cpp kKeyTokens
        for (int i = 0; i < 2; i++) {
            std::string key = actions[i] ? input::KeyName(actions[i]) : std::string();
            if (!key.empty()) labels[kHintIcons[i]] = CapLabel(key);
        }
    }
    std::string pause = input::KeyName("pause"), objectives = input::KeyName("objectives");
    labels[kStart] = pause.empty() ? "Esc" : CapLabel(pause);
    labels[kBack] = objectives.empty() ? "Tab" : CapLabel(objectives);
    SetLabels(labels);
}

}  // namespace

namespace game {

void InitPrompts() { gpu::texture_override::SetProvider(Provide); }

// a file of a pack by its name
bool ReadPackFile(const char* pack, const std::string& name, std::vector<uint8_t>& out) {
    std::map<std::string, Entry> files = ReadDirectory(pack);
    auto it = files.find(name);
    return it != files.end() && ReadAny(pack, it->second.offset, it->second.size, out);
}

// every file of a pack whose name starts as given
std::vector<std::pair<std::string, std::vector<uint8_t>>> ReadPackFiles(const char* pack, const std::string& start) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
    for (const auto& [name, entry] : ReadDirectory(pack)) {
        std::vector<uint8_t> data;
        if (name.compare(0, start.size(), start) == 0 && ReadAny(pack, entry.offset, entry.size, data))
            out.emplace_back(name, std::move(data));
    }
    return out;
}

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

// A controller button for the settings menu's own lines (0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB), as the game shows it
// everywhere else: set 0 the game's own icon (the white button is LB, the black RB), 1 the Modern layout's picture
// (button_images.cpp), 2 the Modern layout's with a PlayStation pad.
// Straight RGBA with a clear texel all round, so it can sit among other pictures.
bool MenuButtonPixels(int button, int set, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    std::vector<uint32_t> argb;  // without the border
    uint32_t w = 0, h = 0;
    if (set) {
        constexpr int kXna[] = {kXnaA, kXnaB, kXnaX, kXnaY, kXnaLB, kXnaRB};
        constexpr int kPs[] = {kPsCross, kPsCircle, kPsSquare, kPsTriangle, kPsL1, kPsR1};
        if (button < 0 || button >= int(std::size(kXna))) return false;
        const ButtonPicture* p = XnaButton(set == 2 ? kPs[button] : kXna[button]);
        if (!p || !p->width || !p->height) return false;
        h = 96;
        w = std::clamp(uint32_t(std::lround(float(h) * float(p->width) / float(p->height))), 16u, 320u);
        Canvas canvas(w, h);
        FitPicture(canvas, *p, 0, 0, float(w), float(h));
        argb = canvas.Finish()->argb;
    } else {
        constexpr Icon kIcons[] = {kA, kB, kX, kY, kWhite, kBlack};
        if (button < 0 || button >= int(std::size(kIcons))) return false;
        std::map<std::string, Entry> files = ReadDirectory();
        std::vector<uint8_t> dds, table;
        auto read = [&](const char* name, std::vector<uint8_t>& out) {
            auto it = files.find(name);
            return it != files.end() && ReadAny(kPack, it->second.offset, it->second.size, out);
        };
        if (!read("i_button_icons_xb_en.dds", dds) || !read("i_button_icons_xb_en.fdf", table)) return false;
        if (dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || std::memcmp(dds.data() + 84, "DXT3", 4) != 0)
            return false;
        uint32_t th = U32(dds, 12), tw = U32(dds, 16), size = Level0Size(dds, tw, th);
        if (!tw || !th || tw > 1024 || th > 1024 || !size || 128 + size > dds.size()) return false;
        std::vector<Color> texels = DecodeDxt3(std::vector<uint8_t>(dds.begin() + 128, dds.begin() + 128 + size), tw, th);
        const Box* box = nullptr;
        std::vector<Box> boxes = IconTable(table);
        for (const Box& b : boxes)
            if (b.icon == kIcons[button]) box = &b;
        if (!box || box->x < 0 || box->y < 0 || box->x + box->width > int(tw) || box->y + box->height > int(th)) return false;
        w = uint32_t(box->width), h = uint32_t(box->height);
        argb.resize(size_t(w) * h);
        auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const Color& c = texels[size_t(uint32_t(box->y) + y) * tw + uint32_t(box->x) + x];
                argb[size_t(y) * w + x] = byte(c.a) << 24 | byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
            }
    }
    width = w + 2, height = h + 2;
    rgba.assign(size_t(width) * height * 4, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = argb[size_t(y) * w + x];
            uint8_t* d = &rgba[(size_t(y + 1) * width + x + 1) * 4];
            d[0] = uint8_t(c >> 16), d[1] = uint8_t(c >> 8), d[2] = uint8_t(c), d[3] = uint8_t(c >> 24);
        }
    return true;
}

// each client frame, in menus and in play
void UpdatePrompts() { Update(); }

}  // namespace game
