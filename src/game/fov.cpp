// Field of view (the F10 panel). cg_fov (vmCvar value at 0x94C3E8; 70 by default, horizontal degrees on a 4:3
// picture) is set through Cvar_SetValue (0x29FDD0, cdecl name, float) once a frame from the client frame, again when a
// level start put the default back. The gun has a scene of its own set up with the view's fov_y (R_RenderView loads
// viewParms fov_y at 0x2475DE): at a wider fov it would shrink and show what the game's 70 keeps off screen, so its
// fov_y is narrowed back by the fov's factor at the hip, blended to the view's own while aiming down the sights.
// Addresses after CoD2-BRO-Native-Loader (GPL-2.0-or-later, THIRD_PARTY_NOTICES.md).

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/options.h"
#include "app/app.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kFovValue = 0x94C3E8;    // cg_fov's vmCvar value
constexpr uint32_t kCvarSetValue = 0x29FDD0;
constexpr uint32_t kFovName = 0x8398C8;     // "cg_fov" in the image
constexpr uint32_t kRefdefFovY = 0x972BA4;  // cg.refdef fov_y
constexpr uint32_t kZoom = 0x9725D8;        // 0 at the hip to 1 down the sights
constexpr uint32_t kEngineFrame = 0x9028E0;
constexpr float kGameFov = 70.0f;
constexpr float kHalfRadians = 3.14159265f / 360.0f;

float F(uint32_t a) { return kernel::G<float>(a); }

uint32_t g_set_frame = 0;

void ApplyFov(X86Context& ctx) {
    float wanted = float(game::ClampFov(game::Opt().fov));
    float current = F(kFovValue);
    uint32_t frame = kernel::R32(kEngineFrame);
    // cgame is up (its vmCvar has a value) and has had a few frames to pick up the last set
    if (current > 0 && current != wanted && frame - g_set_frame > 10) {
        uint32_t bits;
        std::memcpy(&bits, &wanted, 4);
        kernel::CallGuest(ctx, kCvarSetValue, {kFovName, bits});
        g_set_frame = frame;
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002F1E60);

namespace game {
void UpdatePrompts();  // src/game/prompts.cpp
void UpdateHints();    // src/game/hints.cpp
void RunCommands(X86Context& ctx);  // src/game/pad_layout.cpp
}

// the client frame, once a frame from the main loop, in menus and in play
X86_FUNC(sub_002F1E60) {
    __imp__sub_002F1E60(ctx, base);
    X86HostFp host_fp;
    ApplyFov(ctx);
    game::UpdatePrompts();
    game::UpdateHints();
    game::RunCommands(ctx);
}

// R_RenderView, the gun scene's fov_y just loaded into ecx
void GunFov(X86Context& ctx, uint8_t*) {
    float wanted = float(game::ClampFov(game::Opt().fov));
    if (wanted == kGameFov) return;
    X86HostFp host_fp;
    float factor = std::tan(wanted * kHalfRadians) / std::tan(kGameFov * kHalfRadians);
    float zoom = std::clamp(F(kZoom), 0.0f, 1.0f);
    float view = std::tan(F(kRefdefFovY) * kHalfRadians);
    float fov_y = std::atan(view / factor * (1.0f - zoom) + view * zoom) / kHalfRadians;
    if (!std::isfinite(fov_y) || fov_y <= 0) return;
    std::memcpy(&ctx.ecx, &fov_y, 4);
}
