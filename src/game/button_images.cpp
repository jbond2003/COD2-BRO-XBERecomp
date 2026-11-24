// The controller's buttons (button_images/ beside CMakeLists.txt, THIRD_PARTY_NOTICES.md), embedded in the exe:
// A B X Y, Start, Back and the dpad from XNA Game Studio's Button Images sample (its own .tga files, Microsoft
// Permissive License); LB RB LT RT by Nicolae Xelu Berbece (CC0, 360_*.png as .tga). prompts.cpp draws them in place of the game's button icons while a controller is in use with the Modern layout.
// Each is read once and trimmed to what is not transparent.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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

#include "core/log.h"

namespace game {

// the pictures, in this order (src/cursor.rc.in names the resources COD2BRO_XNA_<n>)
enum ButtonImage { kXnaA, kXnaB, kXnaX, kXnaY, kXnaBack, kXnaStart, kXnaDpad, kXnaLB, kXnaRB, kXnaLT, kXnaRT,
                   kXnaCount };

// straight alpha, 0xAARRGGBB, rows top to bottom
struct ButtonPicture {
    uint32_t width = 0, height = 0;
    std::vector<uint32_t> argb;
};

namespace {

constexpr const char* kFiles[kXnaCount] = {
    "xboxControllerButtonA.tga", "xboxControllerButtonB.tga",     "xboxControllerButtonX.tga",
    "xboxControllerButtonY.tga", "xboxControllerBack.tga",        "xboxControllerStart.tga",
    "xboxControllerDPad.tga",    "360_LB.tga",                    "360_RB.tga",
    "360_LT.tga",                "360_RT.tga"};

std::once_flag g_loaded;
std::array<ButtonPicture, kXnaCount> g_pictures;

// a 32 bit Targa, plain or run length coded
bool DecodeTga(const uint8_t* d, size_t size, ButtonPicture& out) {
    if (size < 18) return false;
    uint8_t id = d[0], type = d[2], bpp = d[16], desc = d[17];
    uint32_t w = uint32_t(d[12] | d[13] << 8), h = uint32_t(d[14] | d[15] << 8);
    if (d[1] || (type != 2 && type != 10) || bpp != 32 || !w || !h || w > 1024 || h > 1024) return false;
    std::vector<uint32_t> px;
    px.reserve(size_t(w) * h);
    size_t at = 18 + id;
    auto pixel = [&](size_t i) { return uint32_t(d[i] | d[i + 1] << 8 | d[i + 2] << 16 | uint32_t(d[i + 3]) << 24); };
    while (px.size() < size_t(w) * h) {
        if (type == 2) {
            if (at + 4 > size) return false;
            px.push_back(pixel(at)), at += 4;
            continue;
        }
        if (at >= size) return false;
        uint8_t head = d[at++];
        size_t count = size_t(head & 0x7F) + 1;
        if (head & 0x80) {
            if (at + 4 > size) return false;
            px.insert(px.end(), count, pixel(at)), at += 4;
        } else {
            if (at + count * 4 > size) return false;
            for (size_t i = 0; i < count; i++) px.push_back(pixel(at)), at += 4;
        }
    }
    px.resize(size_t(w) * h);
    if (!(desc & 0x20))  // bottom row first
        for (uint32_t y = 0; y < h / 2; y++)
            std::swap_ranges(px.begin() + size_t(y) * w, px.begin() + size_t(y + 1) * w, px.begin() + size_t(h - 1 - y) * w);
    // trimmed to the part that shows
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            if (px[size_t(y) * w + x] >> 24) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
    if (x1 <= x0 || y1 <= y0) return false;
    out.width = x1 - x0, out.height = y1 - y0;
    out.argb.resize(size_t(out.width) * out.height);
    for (uint32_t y = 0; y < out.height; y++)
        std::memcpy(&out.argb[size_t(y) * out.width], &px[size_t(y + y0) * w + x0], out.width * 4);
    return true;
}

bool ReadBytes(int i, std::vector<uint8_t>& out) {
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(nullptr);
    std::string name = "COD2BRO_XNA_" + std::to_string(i);
    HRSRC resource = FindResourceA(module, name.c_str(), MAKEINTRESOURCEA(10));  // RT_RCDATA
    if (!resource) return false;
    DWORD bytes = SizeofResource(module, resource);
    auto data = static_cast<const uint8_t*>(LockResource(LoadResource(module, resource)));
    if (!data || !bytes) return false;
    out.assign(data, data + bytes);
    return true;
#else
    // off Windows (offline checks): the folder COD2_BUTTON_IMAGES names
    const char* dir = std::getenv("COD2_BUTTON_IMAGES");
    if (!dir) return false;
    std::ifstream f(std::string(dir) + "/" + kFiles[i], std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !out.empty();
#endif
}

void Load() {
    int found = 0;
    for (int i = 0; i < kXnaCount; i++) {
        std::vector<uint8_t> bytes;
        if (ReadBytes(i, bytes) && DecodeTga(bytes.data(), bytes.size(), g_pictures[size_t(i)])) found++;
    }
    if (found == kXnaCount) LOGR("button prompts: the controller's %d button pictures for the Modern layout", found);
    else LOGE("button prompts: only %d of the %d controller button pictures could be read", found, int(kXnaCount));
}

}  // namespace

// one of the pictures, null if it could not be read
const ButtonPicture* XnaButton(int which) {
    std::call_once(g_loaded, Load);
    if (which < 0 || which >= kXnaCount || g_pictures[size_t(which)].argb.empty()) return nullptr;
    return &g_pictures[size_t(which)];
}

}  // namespace game
