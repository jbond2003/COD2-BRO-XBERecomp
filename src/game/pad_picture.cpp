// The Controller screen's picture with the Modern configuration picked: an Xbox One controller (modern_controller.png
// beside CMakeLists.txt, drawn by jojo8479) in place of the game's Xbox controller (xboxlineA.dds of sp/frontend.cod,
// 512x512 DXT3, its lines ending where the screen's labels are). The labels are the screen's text widgets
// (controllerconfigxbox_menu.panel), each found by where the panel puts it; while Modern is picked each is drawn at
// the end of the new picture's line for its button and put back after. The picture is 0.701 screen units a texel
// (the old lines' ends against their labels), so a label moves 0.701 times as far as its line's end did.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

#include "common/menu_state.h"
#include "common/options.h"
#include "core/log.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"

namespace game {
namespace {

using gpu::texture_override::Picture;

constexpr uint32_t kFrontEnd = 0x95F8F8, kInGame = 0x95F8FC;
constexpr uint32_t kControllerScreen = 0x829C48;
constexpr float kScale = 0.701f;  // screen units a texel

// a text widget: x +0x2C, y +0x30, its alignment in flags +0x5C (0x10 left, 0x20 right, neither: centred)
constexpr uint32_t kX = 0x2C, kY = 0x30, kFlags = 0x5C;
constexpr uint32_t kLeft = 0x10, kRight = 0x20, kAlign = 0x30;

struct Move {
    float x, y;            // where the panel puts it
    float to_x, to_y;      // where it goes with Modern picked
    int32_t align;         // -1 as it is, else its new alignment bits
};

// a label (the old line's end and the new one's in texels: right side labels start at the end, left side ones end
// there): the shift is kScale times the end's
constexpr float Dx(float from, float to) { return (to - from) * kScale; }

// the lines from a trigger and its bumper are 9 units apart, less than a label is high: the trigger's label goes this
// far above its line
constexpr float kApart = 5.0f;
constexpr float kSprintWidth = 47.0f;  // "Sprint" at the screen's letter size, and a space

constexpr Move kMoves[] = {
    // right side: RT, X, Y, B, A; the white button's label goes to RB
    {434.3f, 239.3f, 434.3f + Dx(412, 395), 239.3f + Dx(162, 161) - kApart, -1},
    {434.5f, 254.4f, 434.5f + Dx(412, 435), 254.4f + Dx(184, 189), -1},
    {433.8f, 269.8f, 433.8f + Dx(412, 435), 269.8f + Dx(205, 207), -1},
    {433.2f, 286.5f, 433.2f + Dx(412, 435), 286.5f + Dx(229, 229.5f), -1},
    {432.8f, 303.0f, 432.8f + Dx(412, 435), 303.0f + Dx(252, 250.5f), -1},
    {432.2f, 324.4f, 432.2f + Dx(412, 412), 324.4f + Dx(286, 174), -1},
    // the black button's goes to LB, on the left (ending where the left side's labels end)
    {431.8f, 339.6f, 200.0f, 237.8f + Dx(161, 173), kRight},
    // left side: LT, L3 (its "(Click)" before the word: under it, where it was, is the dpad's up), the left stick,
    // the dpad's left and down
    {200.0f, 237.8f, 200.0f + Dx(87, 121), 237.8f + Dx(161, 160) - kApart, -1},
    {188.3f, 258.0f, 188.3f + Dx(87, 102), 258.0f + Dx(189, 245.5f), -1},
    {229.6f, 266.5f, 188.3f + Dx(87, 102) - kSprintWidth, 258.0f + Dx(189, 245.5f) + 1.5f, -1},
    {199.1f, 284.6f, 199.1f + Dx(88, 100), 284.6f + Dx(227, 219.5f), -1},
    {198.4f, 335.8f, 198.4f + Dx(87, 98), 335.8f + Dx(303, 290.5f), -1},
    {198.4f, 352.0f, 198.4f + Dx(87, 98), 352.0f + Dx(317, 307.5f), -1},
    // the dpad's up, above it on the old picture, to the left side
    {283.9f, 225.3f, 200.0f + Dx(87, 101), 237.8f + Dx(161, 265.5f), kRight},
    // Back and Start, on the left, to the tops of their lines: Back's label ends at its line, Start's starts at it
    {195.4f, 306.5f, 283.9f + Dx(205, 231) + 3.0f, 225.3f + Dx(155, 154), kRight},
    {197.3f, 321.4f, 283.9f + Dx(205, 276) - 3.0f, 225.3f + Dx(155, 154), kLeft},
    // under: the dpad's right, the right stick's click ("(Click)" and its word) and the stick
    {305.4f, 363.4f, 305.4f + Dx(220, 228), 363.4f + Dx(340, 345), -1},
    {298.7f, 381.5f, 298.7f + Dx(290, 292), 381.5f + Dx(343, 345), -1},
    {340.4f, 381.5f, 340.4f + Dx(290, 292), 381.5f + Dx(343, 345), -1},
    {394.4f, 359.4f, 394.4f + Dx(315, 310), 359.4f + Dx(334, 340), -1},
};

float RF(uint32_t a) { return kernel::G<float>(a); }

bool ControllerScreenUp() {
    for (uint32_t manager : {kFrontEnd, kInGame}) {
        uint32_t m = kernel::R32(manager);
        if (!m) continue;
        uint32_t screens = kernel::R32(m + 4);
        int32_t active = int32_t(kernel::R32(m + 0xC));
        uint32_t screen = screens && active >= 0 ? kernel::R32(screens + uint32_t(active) * 4) : 0;
        if (screen && kernel::R32(screen) == kControllerScreen) return true;
    }
    return false;
}

std::once_flag g_loaded;
std::shared_ptr<const Picture> g_picture;

#ifdef _WIN32
std::shared_ptr<const Picture> Decode() {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, L"COD2BRO_MODERN_PAD", MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!resource) return nullptr;
    DWORD bytes = SizeofResource(module, resource);
    auto data = static_cast<BYTE*>(LockResource(LoadResource(module, resource)));
    if (!data || !bytes) return nullptr;
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    using Microsoft::WRL::ComPtr;
    std::vector<uint8_t> bgra;
    UINT width = 0, height = 0;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
        if (SUCCEEDED(hr)) hr = wic->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(data, bytes);
        if (SUCCEEDED(hr))
            hr = wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr))
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
        if (SUCCEEDED(hr) && width && height && width <= 2048 && height <= 2048) {
            bgra.resize(size_t(width) * height * 4);
            hr = converter->CopyPixels(nullptr, width * 4, UINT(bgra.size()), bgra.data());
        }
        if (FAILED(hr)) bgra.clear();
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (bgra.empty()) return nullptr;
    auto pic = std::make_shared<Picture>();
    pic->width = width;
    pic->height = height;
    pic->argb.resize(size_t(width) * height);
    std::memcpy(pic->argb.data(), bgra.data(), bgra.size());  // BGRA bytes are 0xAARRGGBB words
    return pic;
}
#else
std::shared_ptr<const Picture> Decode() { return nullptr; }
#endif

}  // namespace

bool ModernPad() { return Opt().modern_layout; }

// render thread (texture_override's provider, src/game/prompts.cpp): the picture while Modern is picked
std::shared_ptr<const Picture> ModernPadPicture() {
    if (!ModernPad()) return nullptr;
    std::call_once(g_loaded, [] {
        g_picture = Decode();
        if (g_picture) LOGR("controller picture: the Modern one, %ux%u", g_picture->width, g_picture->height);
        else LOGW("controller picture: the embedded Modern picture could not be read");
    });
    return g_picture;
}

ModernPadLabel::ModernPadLabel(uint32_t w) {
    if (!ModernPad() || !ControllerScreenUp()) return;
    float x = RF(w + kX), y = RF(w + kY);
    for (const Move& m : kMoves) {
        if (std::fabs(x - m.x) > 0.5f || std::fabs(y - m.y) > 0.5f) continue;
        widget = w;
        own_x = x, own_y = y, own_flags = kernel::R32(w + kFlags);
        kernel::G<float>(w + kX) = m.to_x;
        kernel::G<float>(w + kY) = m.to_y;
        if (m.align >= 0) kernel::W32(w + kFlags, (own_flags & ~kAlign) | uint32_t(m.align));
        return;
    }
}

ModernPadLabel::~ModernPadLabel() {
    if (!widget) return;
    kernel::G<float>(widget + kX) = own_x;
    kernel::G<float>(widget + kY) = own_y;
    kernel::W32(widget + kFlags, own_flags);
}

}  // namespace game
