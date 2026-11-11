// Brightness and the HUD switch.
//
// Brightness: the game sets one gamma ramp, as its device comes up (0x64D34: D3DDevice_SetGammaRamp(0, 0x8A29B0), a
// table in its data, red, green and blue 256 each, close to a straight line). The setting bends that ramp (a gamma
// of brightness / 100) and sets it the same way; at 100 the game's own ramp is never touched.
//
// The HUD: with Hide the HUD on, the crosshair is not drawn (its draw 0x285260, and CG_DrawCrosshair 0x279BE0 in
// it, leave when the cg_drawCrosshair value, [0x9485D4], is 0: it is, for the call), the "Press F to pick up"
// line and its like are not shown (the HUD's hint part, updated by 0x32FD80, hides itself when the cg_cursorHints
// value, [0x94C834], is 0: it is, for the call) and the HUD's pictures and texts are skipped in
// src/game/menu_mouse.cpp. The game's other 2D stays: a mission's title and date card and the subtitles are drawn
// with it, and went with the HUD while this used the game's cg_draw2D switch. That switch is put back on once a
// level, for a profile or session an older build left it off in.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

constexpr uint32_t kGameRamp = 0x8A29B0;
constexpr uint32_t kSetGammaRamp = 0x3E5770;  // (flags, ramp)
constexpr uint32_t kRampBytes = 768;

uint32_t g_ramp = 0;       // ours, in guest memory
uint32_t g_brightness = 100;  // what the picture has
float g_shade = 1.0f;         // and how far faded in (1) or out (0)

// The first start's title picture (menu_mouse.cpp TitleScreen) goes out through black into the profile screen, where
// it cut: the ramp is scaled down, held black while the game changes screens, and brought back up. By the clock, so
// the picture always comes back.
constexpr int64_t kFadeOutMs = 400, kBlackUntilMs = 900, kFadeInMs = 500;
std::atomic<int64_t> g_fade_start{0};  // steady clock ms, 0: none
// and the title picture comes in from black as it starts (held black a moment while the window takes its size)
constexpr int64_t kInBlackMs = 150, kInMs = 700;
std::atomic<int64_t> g_fade_in_start{0};

int64_t SteadyMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

float smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

float ShadeIn() {
    int64_t start = g_fade_in_start.load(std::memory_order_relaxed);
    if (!start) return 1.0f;
    int64_t t = SteadyMs() - start;
    if (t < kInBlackMs) return 0.0f;
    if (t < kInBlackMs + kInMs) return smooth(float(t - kInBlackMs) / kInMs);
    g_fade_in_start.store(0, std::memory_order_relaxed);
    return 1.0f;
}

float Shade() {
    int64_t start = g_fade_start.load(std::memory_order_relaxed);
    if (!start) return ShadeIn();
    int64_t t = SteadyMs() - start;
    if (t < kFadeOutMs) return 1.0f - smooth(float(t) / kFadeOutMs);
    if (t < kBlackUntilMs) return 0.0f;
    if (t < kBlackUntilMs + kFadeInMs) return smooth(float(t - kBlackUntilMs) / kFadeInMs);
    g_fade_start.store(0, std::memory_order_relaxed);
    return 1.0f;
}

void UpdateBrightness(X86Context& ctx) {
    uint32_t want = std::clamp(game::Opt().brightness, 50u, 200u);
    float shade = std::min(Shade(), ShadeIn());
    if (want == g_brightness && shade == g_shade) return;
    if (!g_ramp) g_ramp = mem::AllocSystem(kRampBytes);
    if (!g_ramp) return;
    const uint8_t* from = static_cast<const uint8_t*>(kernel::HostPtr(kGameRamp));
    uint8_t* to = static_cast<uint8_t*>(kernel::HostPtr(g_ramp));
    double power = 100.0 / double(want);
    for (uint32_t i = 0; i < kRampBytes; i++)
        to[i] = uint8_t(std::clamp(std::lround(std::pow(double(from[i]) / 255.0, power) * 255.0 * shade), 0l, 255l));
    kernel::CallGuest(ctx, kSetGammaRamp, {0, g_ramp});
    if (want != g_brightness) LOGR("brightness %u", want);
    g_brightness = want;
    g_shade = shade;
}

void UpdateHud() {
    static bool told = false;
    bool level = !kernel::R32(0x95F8F8) && kernel::R32(0x95F8FC);  // no front end, the in-game menus there
    if (!level) {
        told = false;
        return;
    }
    if (told) return;
    told = true;
    input::QueueCommand("cg_draw2D 1");
}

}  // namespace

namespace game {

// the picture fades to black, stays a moment, and fades back (the first start's title picture into the profile screen)
void StartScreenFade() {
    g_fade_in_start.store(0, std::memory_order_relaxed);
    g_fade_start.store(SteadyMs(), std::memory_order_relaxed);
}
// the picture comes in from black (the first start's title picture)
void StartScreenFadeIn() { g_fade_in_start.store(SteadyMs(), std::memory_order_relaxed); }

// each client frame
void UpdatePcSettings(X86Context& ctx) {
    UpdateBrightness(ctx);
    UpdateHud();
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_00285260);

// the weapon's crosshair (its pictures, and CG_DrawCrosshair 0x279BE0 from in here): with the HUD hidden it finds
// cg_drawCrosshair 0 for the call
X86_FUNC(sub_00285260) {
    constexpr uint32_t kDrawCrosshair = 0x9485D4;  // vm_cg_drawCrosshair's integer
    if (!game::Opt().hide_hud) return __imp__sub_00285260(ctx, base);
    uint32_t own = kernel::R32(kDrawCrosshair);
    kernel::W32(kDrawCrosshair, 0);
    __imp__sub_00285260(ctx, base);
    kernel::W32(kDrawCrosshair, own);
}

X86_EXTERN_FUNC(__imp__sub_0032FD80);

// the HUD's hint part ("Press F to pick up the Thompson ammo", use, mount): with the HUD hidden it finds
// cg_cursorHints 0 for the call and marks itself hidden
X86_FUNC(sub_0032FD80) {
    constexpr uint32_t kCursorHints = 0x94C834;  // vm_cg_cursorHints's integer
    if (!game::Opt().hide_hud) return __imp__sub_0032FD80(ctx, base);
    uint32_t own = kernel::R32(kCursorHints);
    kernel::W32(kCursorHints, 0);
    __imp__sub_0032FD80(ctx, base);
    kernel::W32(kCursorHints, own);
}
