// Sharp text: the game's four bitmap fonts (a sheet texture and a .fdf glyph table each, in the .cod packs) drawn
// again at the output's resolution from font files (font_atlas.h). The font is laid out as itself, its own widths and
// spacing at the original's letter height: the game reads our table where it parses the font's (0x65590) and the
// renderer draws our sheet in place of the old one (gpu/texture_override.h, through the button prompts' provider).
// The game's text code is untouched. The fonts: the real one from Windows where it has it (Impact, Garamond Bold),
// else a free look-alike built into the exe (fonts/, SIL OFL).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "game/font_atlas.h"
#include "gpu/texture.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_00065590);

namespace game {

bool ReadPackFile(const char* pack, const std::string& name, std::vector<uint8_t>& out);  // prompts.cpp

namespace {

using gpu::texture_override::Picture;

// every original is white with (except impact) a one pixel drop shadow to the lower right
struct FontSpec {
    const char* name;      // <name>.dds and <name>.fdf
    const char* pack;
    const char* windows;   // in the Windows fonts folder, the real face; null: none
    const char* resource;  // built in look-alike: RCDATA name, file in fonts/
    const char* file;
    bool shadow;
    uint32_t shadow_rgb;
};
constexpr char kGlobal[] = "D:\\sp\\global.cod", kFrontEnd[] = "D:\\sp\\frontend.cod";
constexpr FontSpec kFonts[] = {
    {"garamond", kGlobal, "GARABD.TTF", "COD2BRO_FONT_GARAMOND", "EBGaramond-Bold.ttf", true, 0x080808},  // menus, objectives
    {"GemFontOne", kGlobal, nullptr, "COD2BRO_FONT_JOST", "Jost-500-Medium.ttf", true, 0x520052},         // Futura-like
    {"arialblack_14", kGlobal, "GARABD.TTF", "COD2BRO_FONT_GARAMOND", "EBGaramond-Bold.ttf", true, 0x080808},  // a bold Garamond despite the name
    {"impact", kFrontEnd, "impact.ttf", "COD2BRO_FONT_ANTON", "Anton-Regular.ttf", false, 0},             // front end headings
};

struct Font {
    const FontSpec* spec = nullptr;
    font_atlas::Sheet sheet;
    std::vector<uint8_t> level0;  // the original sheet's bytes as the game holds them
    std::vector<uint8_t> fdf;     // the original table's text
    std::vector<uint8_t> ttf;
    std::string from;
    font_atlas::Layout layout;    // the font laid out as itself; no fdf text: it did not fit, glyphs go in the old boxes
    uint32_t guest_fdf = 0;       // the layout's table in guest memory
    bool typeset = false;         // the game read our table: the sheet is drawn to it
    std::map<int, std::shared_ptr<const Picture>> by_scale;
};

std::once_flag g_loaded;
std::mutex g_lock;
std::vector<Font> g_fonts;

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {}) : std::vector<uint8_t>();
}

bool IsFont(const std::vector<uint8_t>& ttf) {
    stbtt_fontinfo info;
    return ttf.size() > 12 && stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0));
}

std::vector<uint8_t> BuiltIn(const FontSpec& spec) {
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceA(module, spec.resource, MAKEINTRESOURCEA(10));  // RT_RCDATA
    if (!resource) return {};
    DWORD bytes = SizeofResource(module, resource);
    auto data = static_cast<const uint8_t*>(LockResource(LoadResource(module, resource)));
    return data && bytes ? std::vector<uint8_t>(data, data + bytes) : std::vector<uint8_t>();
#else
    return ReadFile(std::string("fonts/") + spec.file);
#endif
}

void ReadTtf(Font& font) {
#ifdef _WIN32
    if (font.spec->windows) {
        const char* windir = std::getenv("WINDIR");
        font.ttf = ReadFile(std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\" + font.spec->windows);
        if (IsFont(font.ttf)) {
            font.from = font.spec->windows;
            return;
        }
    }
#endif
    font.ttf = BuiltIn(*font.spec);
    font.from = font.spec->file;
    if (!IsFont(font.ttf)) font.ttf.clear();
}

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

void Load() {
    for (const FontSpec& spec : kFonts) {
        std::vector<uint8_t> dds, fdf;
        if (!ReadPackFile(spec.pack, std::string(spec.name) + ".dds", dds) ||
            !ReadPackFile(spec.pack, std::string(spec.name) + ".fdf", fdf) || dds.size() <= 128 ||
            std::memcmp(dds.data(), "DDS ", 4) != 0)
            continue;
        Font font;
        font.spec = &spec;
        font.sheet.height = U32(dds, 12);
        font.sheet.width = U32(dds, 16);
        font.sheet.glyphs = font_atlas::ParseFdf(fdf);
        font.sheet.shadow = spec.shadow;
        font.sheet.shadow_rgb = spec.shadow_rgb;
        if (font.sheet.glyphs.empty() || !font.sheet.width || !font.sheet.height || font.sheet.width > 1024 ||
            font.sheet.height > 1024)
            continue;
        font.level0.assign(dds.begin() + 128, dds.end());  // level 0 first, compared up to the texture's level 0
        ReadTtf(font);
        if (font.ttf.empty()) {
            LOGW("sharp text: no font file for %s, it keeps its original letters", spec.name);
            continue;
        }
        font.fdf = std::move(fdf);
        if (!font_atlas::Typeset(font.sheet, font.fdf, font.ttf, font.layout)) font.layout.fdf.clear();
        LOGR("sharp text: %s from %s%s", spec.name, font.from.c_str(),
             font.layout.fdf.empty() ? ", in the original's boxes" : "");
        g_fonts.push_back(std::move(font));
    }
    LOGR("sharp text: %zu of %zu fonts", g_fonts.size(), std::size(kFonts));
}

}  // namespace

// for the texture override provider: the redrawn sheet when the texture is one of the game's font sheets, else null
std::shared_ptr<const Picture> SharpText(const gpu::TextureDesc& d, const uint8_t* guest, float render_scale) {
    std::call_once(g_loaded, Load);
    for (Font& font : g_fonts) {
        uint32_t size = gpu::LevelSize(d, 0);
        if (d.width != font.sheet.width || d.height != font.sheet.height || !size || size > font.level0.size() ||
            std::memcmp(guest, font.level0.data(), size) != 0)
            continue;
        // at least twice the original, so the letters stay smooth at every output size; at most 8 times
        // (the front end's screens are shown larger than the frame's own scale: a band of it over the whole picture,
        // src/game/menu_mouse.cpp)
        constexpr float kBandZoom = 480.0f / 347.0f;
        int scale = std::clamp(int(std::ceil(render_scale * kBandZoom - 0.01f)), 2, 8);
        std::lock_guard lock(g_lock);
        auto& cached = font.by_scale[scale];
        if (!cached) {
            int missing = 0;
            auto picture = std::make_shared<Picture>();
            picture->width = font.sheet.width * scale;
            picture->height = font.sheet.height * scale;
            picture->argb = font_atlas::Draw(font.sheet, font.ttf, scale, &missing, font.typeset ? &font.layout : nullptr);
            if (picture->argb.empty()) return nullptr;
            LOGR("sharp text: %s drawn at %ux%u (%d glyphs not in the font)", font.spec->name, picture->width,
                 picture->height, missing);
            cached = std::move(picture);
        }
        return cached;
    }
    return nullptr;
}

// our table for the font whose table text this is, 0 for another font's
uint32_t TypesetTable(uint32_t text) {
    std::call_once(g_loaded, Load);
    const char* given = static_cast<const char*>(kernel::HostPtr(text));
    for (Font& font : g_fonts) {
        if (font.layout.fdf.empty() || font.fdf.empty() ||
            std::strncmp(given, reinterpret_cast<const char*>(font.fdf.data()), font.fdf.size()) != 0)
            continue;
        std::lock_guard lock(g_lock);
        if (!font.guest_fdf) {
            font.guest_fdf = mem::AllocSystem(uint32_t(font.layout.fdf.size()) + 1);
            if (!font.guest_fdf) return 0;
            std::memcpy(kernel::HostPtr(font.guest_fdf), font.layout.fdf.c_str(), font.layout.fdf.size() + 1);
        }
        if (!font.typeset) {
            font.typeset = true;
            font.by_scale.clear();
            gpu::texture_override::Changed();
            LOGR("sharp text: %s set in its own widths", font.spec->name);
        }
        return font.guest_fdf;
    }
    return 0;
}

}  // namespace game

// Font table parse (edi font, text): numbers after the keywords of a .fdf's text into the font's glyph table (+0x28,
// 0x1C a glyph) and its texture coordinates (+0x4C). Ours for the fonts drawn again.
X86_FUNC(sub_00065590) {
    uint32_t slot = ctx.esp + 4, text = kernel::R32(slot);
    uint32_t mine = text ? game::TypesetTable(text) : 0;
    if (mine) kernel::W32(slot, mine);
    __imp__sub_00065590(ctx, base);
    if (mine) kernel::W32(slot, text);  // the caller's own
}
