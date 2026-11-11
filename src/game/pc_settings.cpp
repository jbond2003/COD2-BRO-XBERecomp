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

void UpdateBrightness(X86Context& ctx) {
    uint32_t want = std::clamp(game::Opt().brightness, 50u, 200u);
    if (want == g_brightness) return;
    if (!g_ramp) g_ramp = mem::AllocSystem(kRampBytes);
    if (!g_ramp) return;
    const uint8_t* from = static_cast<const uint8_t*>(kernel::HostPtr(kGameRamp));
    uint8_t* to = static_cast<uint8_t*>(kernel::HostPtr(g_ramp));
    double power = 100.0 / double(want);
    for (uint32_t i = 0; i < kRampBytes; i++)
        to[i] = uint8_t(std::clamp(std::lround(std::pow(double(from[i]) / 255.0, power) * 255.0), 0l, 255l));
    kernel::CallGuest(ctx, kSetGammaRamp, {0, g_ramp});
    g_brightness = want;
    LOGR("brightness %u", want);
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
