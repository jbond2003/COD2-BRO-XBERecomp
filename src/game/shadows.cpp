// Soldiers' weapons in their shadows.
//
// R_AddDObjSurfaces (0x244020) adds each part of a model to the scene and, for one that casts a shadow, a copy to
// the shadow picture's scene ([0x95F6CC]). A skinned part's copy takes the model's matrix, which is right: its bones
// go with it. A rigid part (the rifle in a soldier's hands, a jeep's wheels) is placed by its bone: the scene's copy gets the
// bone's matrix times the model's (the call at 0x245399), but the shadow's copy is given the model's matrix alone
// (0x24558D pushes it), so a rifle's shadow lies at the soldier's feet (some 45 units below his hands, measured in the
// prologue) and none shows where the rifle is. Here the shadow's copy takes the matrix the scene's copy had.

#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

// the model's matrix just pushed for nglListAddMesh ([esp]); the part's own, bone and all, is at the frame's +0xA0,
// which is esp + 0xAC after the call's three pushes so far
void RigidShadowMatrix(X86Context& ctx, uint8_t*) {
    kernel::W32(ctx.esp, ctx.esp + 0xAC);
}

// Shadows that fall away from the sun.
//
// The game's shadows are one picture (512 x 512, 700 units across) of the silhouettes of the soldiers and vehicles
// near the camera, seen from straight above and laid on the world straight down (R_RenderTestShadow 0x241290): every
// shadow is a blot under its owner, whatever the light. The picture's camera (16 floats at 0x988730: across, up,
// the way it looks, where it is) is used both to draw the picture and to lay it on the world, so turning it turns
// both. Here it looks along the level's sun instead, just before the function uses it (0x241636).
//
// The sun is in the level's light grid (R_GetCellLighting 0x23E510: position in eax, returns the grid of its cell):
// lights of 0x1C bytes (the way the light travels, +0xC 0 for a directional one, colour), samples that name up to
// three lights each with a weight, in areas (0x20 bytes: origin, cell size, width and height in samples, the first
// sample). The directional light with the most weight at the camera is the sun. Where none reaches (indoors)
// the shadows ease back to straight down. Graphics, Sun Shadows: off unless chosen, on in the Remastered preset.

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/options.h"

namespace {

using namespace kernel;

constexpr uint32_t kShadowCamera = 0x988730;
constexpr uint32_t kViewOrigin = 0x95E240;
constexpr uint32_t kCellLighting = 0x23E510;
constexpr uint32_t kShadowLift = 0x8DB1A0;   // the camera's height over the eye, 100
constexpr float kShadowReach = 700.0f;  // parts this near the camera cast a shadow though out of the view
constexpr float kLeastDrop = 0.6f;  // the sun is taken no lower than this (37 degrees up): longer shadows leave the picture

struct {
    float sun[3] = {};  // the way its light travels (down: z below 0), 0 while none is known
    uint32_t grid = 0;  // the grid it was read from: another level's is another sun
    float lean = 0;     // 0 straight down to 1 along the sun, eased
    std::chrono::steady_clock::time_point at;
} g_sun;

// the directional light with the most weight in the sample nearest the point; false where none reaches
bool FindSun(uint32_t grid, const float* at) {
    uint32_t lights = R32(grid), samples = R32(grid + 8), refs = R32(grid + 0x10), areas = R32(grid + 0x18);
    uint32_t count = R32(grid + 0x1C);
    if (!lights || !samples || !refs || !areas || count > 4096) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t a = areas + i * 0x20;
        float sx = G<float>(a + 0xC), sy = G<float>(a + 0x10);
        uint32_t w = G<uint16_t>(a + 0x14), h = G<uint16_t>(a + 0x16);
        if (!(sx > 0) || !(sy > 0) || !w || !h) continue;
        float fx = (at[0] - G<float>(a)) / sx, fy = (at[1] - G<float>(a + 4)) / sy;
        if (fx < 0 || fy < 0 || fx > float(w - 1) || fy > float(h - 1)) continue;
        uint32_t sample = R32(samples + (R32(a + 0x18) + uint32_t(fy + 0.5f) * w + uint32_t(fx + 0.5f)) * 4);
        uint32_t n = (sample >> 15) & 3, first = sample >> 17, best = 0;
        for (uint32_t r = 0; r < n; r++) {
            uint32_t light = lights + G<uint16_t>(refs + (first + r) * 4) * 0x1C;
            uint32_t weight = G<uint16_t>(refs + (first + r) * 4 + 2);
            if (G<float>(light + 0xC) != 0 || weight <= best) continue;
            float x = G<float>(light), y = G<float>(light + 4), z = G<float>(light + 8);
            float len = std::sqrt(x * x + y * y + z * z);
            if (!(len > 0.001f) || !(z < 0)) continue;
            best = weight;
            g_sun.sun[0] = x / len, g_sun.sun[1] = y / len, g_sun.sun[2] = z / len;
        }
        if (best) return true;
    }
    return false;
}

}  // namespace

// R_RenderTestShadow, its camera just worked out
void SunShadowCamera(X86Context& ctx, uint8_t*) {
    X86HostFp host_fp;
    uint32_t grid;
    {
        // every register as it was after: the function goes on with what it has in them
        X86Context kept = ctx;
        uint32_t top = R32(ctx.esp);
        ctx.eax = kViewOrigin, ctx.ecx = 0;
        grid = CallGuest(ctx, kCellLighting, {});
        ctx = kept;
        W32(ctx.esp, top);
    }
    if (grid != g_sun.grid) g_sun = {}, g_sun.grid = grid;
    bool lit = game::Opt().sun_shadows && grid && FindSun(grid, &G<float>(kViewOrigin));
    // Out of the sun (indoors, or the setting off) the shadows ease back to straight down, and out again in it.
    auto now = std::chrono::steady_clock::now();
    float passed = std::clamp(std::chrono::duration<float>(now - g_sun.at).count(), 0.0f, 0.1f);
    g_sun.at = now;
    g_sun.lean += std::clamp((lit ? 1.0f : 0.0f) - g_sun.lean, -passed * 1.5f, passed * 1.5f);
    if (!(g_sun.sun[2] < 0) || !(g_sun.lean > 0)) return;  // straight down, as the game has it
    // the way the light travels, no flatter than kLeastDrop
    float lx = g_sun.sun[0], ly = g_sun.sun[1], lz = g_sun.sun[2];
    if (lz > -kLeastDrop) {
        float flat = std::sqrt(lx * lx + ly * ly), most = std::sqrt(1 - kLeastDrop * kLeastDrop);
        lx *= most / flat, ly *= most / flat, lz = -kLeastDrop;
    }
    lx *= g_sun.lean, ly *= g_sun.lean;
    float unit = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= unit, ly /= unit, lz /= unit;
    float* cam = &G<float>(kShadowCamera);
    float ux = cam[4], uy = cam[5];  // the picture's up: level, by the view
    // across = up x look, up = look x across: as the game builds them for a look straight down
    float ax = uy * lz, ay = -ux * lz, az = ux * ly - uy * lx;
    float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (!(len > 0.001f)) return;
    ax /= len, ay /= len, az /= len;
    float vx = ly * az - lz * ay, vy = lz * ax - lx * az, vz = lx * ay - ly * ax;
    cam[0] = ax, cam[1] = ay, cam[2] = az, cam[3] = 0;
    cam[4] = vx, cam[5] = vy, cam[6] = vz, cam[7] = vx;
    cam[8] = lx, cam[9] = ly, cam[10] = lz, cam[11] = 0;
    // as high over the eye as before, back along the light. The picture is laid on the world for 500 units along the
    // light from there (nglAddShadowProjector 0x704F0), which reaches 200 units below the eye at the lowest sun.
    float back = G<float>(kShadowLift) / -lz;
    cam[12] -= lx * back, cam[13] -= ly * back;
}

// Shadows in the shape of what casts them.
//
// The game makes the shadow's copy of a model from its coarsest level of detail (DObj_GetCoarsestLod 0x29520 for a
// skinned part, called at 0x2447E2; DObj_SetForcedLod 0x14610 with 4 for a rigid one, at 0x24551E): a few dozen
// triangles, which pass for a soldier as a blot under his feet and not as a shadow lying beside him. And it makes
// the copy only of a part it has just put in the view (nglListAddMesh 0x68850 at 0x244663 and 0x245399 returning
// above 0), so a tank half out of the picture casts half a shadow and a soldier just off screen none.
// With Sun Shadows on the copy is of the level of detail being drawn, and a part out of the view but near the
// camera counts as added, so the function goes on to its shadow.

X86_EXTERN_FUNC(__imp__sub_00029520);
X86_EXTERN_FUNC(__imp__sub_00014610);
X86_EXTERN_FUNC(__imp__sub_00068850);

X86_FUNC(sub_00029520) {
    uint32_t dobj = ctx.edx;
    if (game::Opt().sun_shadows && R32(ctx.esp) == 0x2447E7 && dobj) {
        int32_t forced = int32_t(R32(dobj + 0x94)), least = int32_t(R32(dobj + 0x98)), lod = int32_t(R32(dobj + 0x90));
        int32_t drawn = forced >= 0 ? forced : least >= 0 ? least : lod;
        if (drawn >= 0 && drawn < 5) {
            ctx.eax = uint32_t(drawn);
            ctx.esp += 4;  // the return
            return;
        }
    }
    __imp__sub_00029520(ctx, base);
}

X86_FUNC(sub_00014610) {
    if (game::Opt().sun_shadows && R32(ctx.esp) == 0x245523 && ctx.eax == 4) {
        ctx.esp += 4;  // not forced: the copy is of the level being drawn
        return;
    }
    __imp__sub_00014610(ctx, base);
}

X86_FUNC(sub_00068850) {
    uint32_t back = R32(ctx.esp), mesh = R32(ctx.esp + 4), matrix = R32(ctx.esp + 8);
    __imp__sub_00068850(ctx, base);
    if ((back != 0x244668 && back != 0x24539E) || int32_t(ctx.eax) > 0 || !mesh || !matrix) return;
    if (!game::Opt().sun_shadows) return;
    X86HostFp host_fp;
    const float* at = &G<float>(matrix + 0x30);
    const float* eye = &G<float>(kViewOrigin);
    float x = at[0] - eye[0], y = at[1] - eye[1], z = at[2] - eye[2];
    if (x * x + y * y + z * z < kShadowReach * kShadowReach) ctx.eax = 1;
}
