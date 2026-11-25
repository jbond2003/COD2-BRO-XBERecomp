// The muzzle flash on the gun's muzzle. The gun in hand is drawn in a scene of its own with a narrower field of view
// than the world's at the hip (a y scale of 2.14 against 1.90), and the flash is an effect in the world's scene at the
// flash tag's place: so the flash showed nearer the middle of the screen than the muzzle it belongs to, in front of
// the gun. The tag's place asked for the gun in hand (0x27F920: eax the tag's name, edi the gun's model, one argument
// the matrix out, its origin at +0x24) is moved out from the line of sight by the two scenes' ratio, so that the
// world's scene shows it where the gun's shows the tag. The scales are taken as the scenes are set up
// (nglMatrixPerspective 0x5EC20: out, x scale, y scale, near, far; the gun's near plane is 0.1, the world's 4.5).

#include <cmath>

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

using kernel::G;

constexpr uint32_t kViewOrigin = 0x972BB0;
constexpr uint32_t kViewForward = 0x972BC0;

float g_gun_scale = 0, g_world_scale = 0;  // the y scales of the last gun scene and world scene

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0005EC20);
X86_FUNC(sub_0005EC20) {
    {
        X86HostFp host_fp;
        float near_plane = G<float>(ctx.esp + 16), y_scale = G<float>(ctx.esp + 12);
        if (near_plane < 0.2f) g_gun_scale = y_scale;
        else if (near_plane > 2.0f) g_world_scale = y_scale;
    }
    __imp__sub_0005EC20(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0027F920);
X86_FUNC(sub_0027F920) {
    uint32_t out = kernel::R32(ctx.esp + 4);
    __imp__sub_0027F920(ctx, base);
    if (!ctx.eax || !out) return;
    X86HostFp host_fp;
    if (!(g_gun_scale > 0.01f && g_world_scale > 0.01f)) return;
    float k = g_gun_scale / g_world_scale;
    if (!std::isfinite(k) || k < 0.25f || k > 4) return;
    float rel[3], along = 0;
    for (int i = 0; i < 3; i++) {
        rel[i] = G<float>(out + 0x24 + i * 4) - G<float>(kViewOrigin + i * 4);
        along += rel[i] * G<float>(kViewForward + i * 4);
    }
    for (int i = 0; i < 3; i++) {
        float ahead = G<float>(kViewForward + i * 4) * along;
        G<float>(out + 0x24 + i * 4) = G<float>(kViewOrigin + i * 4) + ahead + (rel[i] - ahead) * k;
    }
}
