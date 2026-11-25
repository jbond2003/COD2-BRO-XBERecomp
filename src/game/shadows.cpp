// Soldiers' weapons in their shadows.
//
// R_AddDObjSurfaces (0x244020) adds each part of a model to the scene and, for one that casts a shadow, a copy to
// the shadow picture's scene ([0x95F6CC]). A skinned part's copy takes the model's matrix, which is right: its bones
// go with it. A rigid part (the rifle in a soldier's hands, a jeep's wheels) is placed by its bone: the scene's copy gets the
// bone's matrix times the model's (the call at 0x245399), but the shadow's copy is given the model's matrix alone
// (0x24558D pushes it), so a rifle's shadow lies at the soldier's feet (some 45 units below his hands, measured in the
// prologue) and none shows where the rifle is. Here the shadow's copy takes the matrix the scene's copy had.

#include "cpu/x86_ops.h"
#include "gpu/backend.h"
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
constexpr uint32_t kShadowWidth = 0x8DB19C;  // the picture's width in the world, 700 (a level's script can set it)
constexpr uint32_t kAhead = 0x83F034;        // 0.45
constexpr uint32_t kShadowDepth = 0x8DB1A4;  // how far along its look the picture is laid on the world, 500
constexpr float kOwnDepth = 500.0f;
constexpr uint32_t kBandBelow = 0x8DB124;   // how far under the camera a thing not animated still casts a shadow, 100
constexpr uint32_t kTracePoint = 0x286D00;   // CG_TracePoint: eax mask, ecx end; result, start, mins, maxs, skip
constexpr uint32_t kClientNum = 0x9725CC;
constexpr float kShadowReach = 700.0f;  // parts this near the camera cast a shadow though out of the view
constexpr float kLeastDrop = 0.6f;  // the sun is taken no lower than this (37 degrees up): longer shadows leave the picture

struct {
    float sun[3] = {};  // the way its light travels (down: z below 0), 0 while none is known
    uint32_t grid = 0;  // the grid it was read from: another level's is another sun
    float lean = 0;     // 0 straight down to 1 along the sun, eased
    std::chrono::steady_clock::time_point at;
    float height = 60;  // the eye over the ground, eased
} g_sun;
float g_own_width = 0;  // the picture's width as the game or its level has it
float g_laid = 0;  // how deep along its look the turned picture is laid on the world (0: the game's own 500)
float g_down = 500;  // how deep the picture is laid when it looks straight down: to the ground and under it

// Shadow Distance (Graphics): the picture's width times this
float Farther() {
    uint32_t d = game::Opt().shadow_distance;
    return d == 1 ? 3.0f : d >= 2 ? 6.0f : 1.0f;
}

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
    // the width for the frames to come (this one has used it already), and the game's own depth unless leaning
    float used = G<float>(kShadowWidth);  // what this frame's camera was placed by
    if (g_own_width == 0) g_own_width = used;
    float farther = Farther();
    // the picture (512 across) spread over that much more ground showed its squares: the renderer makes it larger
    gpu::g_boost_size.store(512, std::memory_order_relaxed);
    gpu::g_boost.store(farther, std::memory_order_relaxed);
    if (g_own_width > 0) G<float>(kShadowWidth) = g_own_width * farther;
    G<float>(kShadowDepth) = kOwnDepth;
    bool lit = game::Opt().sun_shadows && grid && FindSun(grid, &G<float>(kViewOrigin));
    // Out of the sun (indoors, or the setting off) the shadows ease back to straight down, and out again in it.
    auto now = std::chrono::steady_clock::now();
    float passed = std::clamp(std::chrono::duration<float>(now - g_sun.at).count(), 0.0f, 0.1f);
    g_sun.at = now;
    g_sun.lean += std::clamp((lit ? 1.0f : 0.0f) - g_sun.lean, -passed * 1.5f, passed * 1.5f);
    // How high the eye is over the ground (a soldier's 60 units, a tank's camera a few hundred, more over a slope):
    // a trace straight down from it, eased so that a wall's top passing under the eye does not make the shadows jump.
    {
        constexpr float kDown = 2000.0f;
        X86Context kept = ctx;
        uint32_t top = R32(ctx.esp);
        ctx.esp -= 0x80;  // start, end, a point for mins and maxs, the result (0x40)
        uint32_t start = ctx.esp, end = ctx.esp + 0x10, zero = ctx.esp + 0x20, result = ctx.esp + 0x30;
        const float* eye = &G<float>(kViewOrigin);
        for (int i = 0; i < 3; i++) G<float>(start + i * 4) = G<float>(end + i * 4) = eye[i], W32(zero + i * 4, 0);
        G<float>(end + 8) = eye[2] - kDown;
        W32(result + 0x20, 0x3F800000);
        ctx.eax = 0x11, ctx.ecx = end;
        CallGuest(ctx, kTracePoint, {result, start, zero, zero, R32(kClientNum)});
        float part = G<float>(result + 0x20);
        ctx = kept;
        W32(ctx.esp, top);
        float height = part > 0.0f && part < 1.0f ? part * kDown : 60.0f;
        g_sun.height += std::clamp(height - g_sun.height, -passed * 400.0f, passed * 400.0f);
    }
    // A thing that is not animated (a wreck, a crate, a body that has come to rest) casts a shadow only within a band
    // of heights about the camera, 100 units under it to 40 over (0x8DB124, 0x8DB120, read by R_AddDObjSurfaces at
    // 0x24442A): right for a soldier's eye, but a tank's camera is higher over the ground than that, and nothing
    // lying on it cast a shadow. The band goes down to as far under the ground as it did from a soldier's eye.
    G<float>(kBandBelow) = std::max(100.0f, g_sun.height + 40.0f * farther);
    // A cutscene's camera can be several hundred units over the ground: the game's 500 from 100 over the eye then
    // ended above the ground and nothing on it had a shadow. The picture reaches as far under the ground as it did
    // from a soldier's eye.
    g_down = std::max(kOwnDepth, G<float>(kShadowLift) + g_sun.height + (kOwnDepth - 160.0f));
    G<float>(kShadowDepth) = g_down;
    g_laid = 0;
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
    // The game puts the camera, the picture's middle, ahead of the eye: kAhead of the width along its up and as far
    // back along its across, which is the way the player looks. The eye is then just inside the picture's corner.
    // A turned picture covers the ground as a slanted shape that the eye is not always in (near shadows went as the
    // player turned), but always the circle of the width about its middle: so the middle is put less far ahead, by
    // how far the picture is turned, and the eye is well inside that circle.
    constexpr float kTurnedAhead = 0.4f;
    float game_ahead = G<float>(kAhead) * 1.41421356f;
    float fx = ux - cam[0], fy = uy - cam[1];
    float flen = std::sqrt(fx * fx + fy * fy);
    if (!(flen > 0.001f)) return;
    float pull = (kTurnedAhead - game_ahead) * g_sun.lean * used / flen;
    cam[12] += fx * pull, cam[13] += fy * pull;
    // across = up x look, up = look x across: as the game builds them for a look straight down
    float ax = uy * lz, ay = -ux * lz, az = ux * ly - uy * lx;
    float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (!(len > 0.001f)) return;
    ax /= len, ay /= len, az /= len;
    float vx = ly * az - lz * ay, vy = lz * ax - lx * az, vz = lx * ay - ly * ax;
    cam[0] = ax, cam[1] = ay, cam[2] = az, cam[3] = 0;
    cam[4] = vx, cam[5] = vy, cam[6] = vz, cam[7] = vx;
    cam[8] = lx, cam[9] = ly, cam[10] = lz, cam[11] = 0;
    // The picture is laid on the world as a box from the camera along its look. Level ground under a turned
    // picture is nearer the camera at the edge towards the sun and further at the other, by "edge" along the look: so
    // the camera goes back along the light from the ground under the picture's middle by that much and the eye's
    // height, with some to spare for ground that rises towards the sun, and the box is as deep again on the far side
    // (g_laid; the game's own is 500 deep whatever the width, which with the light's slant left shadows only on a
    // strip of ground through the picture's middle: they came and went as the player turned).
    constexpr float kOver = 200.0f, kUnder = kOwnDepth - 160.0f;  // ground higher, and lower, than under the eye
    float lift = G<float>(kShadowLift);
    float edge = 0.5f * used * (std::fabs(az) + std::fabs(vz)) / -lz;
    float back = (lift + g_sun.height + kOver) / -lz + edge;
    cam[12] -= lx * back, cam[13] -= ly * back;
    cam[14] += kOver - lz * edge;
    g_laid = back + edge + kUnder / -lz;
    G<float>(kShadowDepth) = g_laid;
}

// nglAddShadowProjector: a light's number, the camera, the box (width, minus the height, the depth, the width), ...
// R_RenderTestShadow passes 500 for the depth.
//
// The projector it makes (0x140 bytes from the frame's store, [0x9025A8] brought up to 16) starts with the matrix
// that gives a point of the world its place in the picture, four numbers: across and up (0 to 1 over the width),
// and two that grow with the depth from the camera along its look, one by the box's depth and one from a half by
// the box's last width. For the game's own picture, straight down from 100 units over the eye, those two are a
// point's height under the camera: 0.32 and 0.73 on the ground a soldier stands on, the same wherever he looks.
// For a picture turned to the sun, or a wider one, they were neither: the ground under the player came out
// anywhere from near the camera to well past the box's end as he turned, and the shadows on it went with the
// headings that put it far. Here the two are given back their meaning, height: what the game has on the ground
// under the eye, changing with the height above or below it, as slowly as the picture is wider.
X86_EXTERN_FUNC(__imp__sub_000704F0);
X86_FUNC(sub_000704F0) {
    bool ours = R32(ctx.esp) == 0x241849;
    uint32_t box = ours ? R32(ctx.esp + 0xC) : 0;
    uint32_t projector = (R32(0x9025A8) + 15) & ~15u;
    if (box) {
        X86HostFp host_fp;
        G<float>(box + 8) = g_laid > 0 ? g_laid : g_down;
    }
    __imp__sub_000704F0(ctx, base);
    float farther = Farther();
    if (!box || !projector || !(g_own_width > 0) || (!(g_laid > 0) && farther == 1.0f && g_down == kOwnDepth)) return;
    X86HostFp host_fp;
    float under = G<float>(kShadowLift) + g_sun.height;        // the game's camera over the ground
    float ground = G<float>(kViewOrigin + 8) - g_sun.height;  // the ground under the eye
    float by[2] = {kOwnDepth * farther, g_own_width * farther};
    float at[2] = {under / kOwnDepth, 0.5f + under / g_own_width};
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t column = projector + 8 + i * 4;
        G<float>(column) = 0, G<float>(column + 0x10) = 0;
        G<float>(column + 0x20) = -1.0f / by[i];
        G<float>(column + 0x30) = at[i] + ground / by[i];
    }
}

// Tanks and trucks. The game adds a vehicle to the scene (0x246F67) with "casts no shadow" (the last of
// R_AddDObjSurfaces' six arguments, pushed as 0): under a blot laid straight down a tank hid its own. With the
// shadows falling away from the sun, or reaching further than the game's own, a vehicle casts one like a soldier.
void VehicleShadow(X86Context& ctx, uint8_t*) {
    if (game::Opt().sun_shadows || game::Opt().shadow_distance) W32(ctx.esp + 0x14, 1);
}

// a level's script sets the picture's width (GScr slot 129)
X86_EXTERN_FUNC(__imp__sub_002F4BE0);
X86_FUNC(sub_002F4BE0) {
    {
        X86HostFp host_fp;
        float width = G<float>(ctx.esp + 4);
        if (width > 0) g_own_width = width;
    }
    __imp__sub_002F4BE0(ctx, base);
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
    float reach = kShadowReach * Farther();
    if (x * x + y * y + z * z < reach * reach) ctx.eax = 1;
}
