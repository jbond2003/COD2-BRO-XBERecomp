// Ambient occlusion (Graphics, off unless chosen): soft shade where surfaces meet, worked out by the renderer from
// the depth of what is drawn.
//
// The game draws through NGL scenes: nglRenderSceneTree (0x74330) draws the current scene's ([0x90259C]) solid list
// (the call at 0x743F6), then its translucent one (0x7446D): smoke, glass, the glow. The shade goes in between, so
// it lies on the world, the soldiers and the vehicles and under everything see-through; the gun's own scene (shaded
// the same way, on a smaller scale) and the HUD come later still. The world's scene is the one in perspective that sees far (8192 units in the prologue; the
// gun's sees 128, the sky's has no solid nodes, a shadow's is not in perspective). Counting its solid nodes instead
// lost the shade wherever few were in view: facing a wall inside a room.
// A scene: +0x10 its camera to screen matrix (nglMatrixPerspective 0x5EC20, D3D's row vectors), +0x300 1 for
// perspective, +0x30C the solid nodes, +0x384 fog on (byte), +0x388 the fog's near and far depths, its least and
// most (nglSetFog 0x5E4D0), +0x3A0 its colour (0 to 1).

#include <cmath>

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace {

constexpr uint32_t kScene = 0x90259C;
constexpr uint32_t kFrontEnd = 0x95F8F8;
constexpr float kWorldFar = 500.0f;  // a scene that sees less far is not the world's
constexpr float kGunFar = 32.0f;     // nor is one that sees less far than this the gun's
constexpr float kGunRadius = 3.0f;   // the gun and the hands are shaded from this near: a finger is about one unit thick
constexpr float kRadius = 28.0f;      // how far a surface is shaded from: the game's units are about an inch
constexpr float kStrength = 1.4f;     // 30% lighter than 2.0 (his ask): 1.4x the shade the shaders work out

}  // namespace

// nglRenderSceneTree, the scene's solid list just drawn
void AmbientOcclusion(X86Context&, uint8_t*) {
    using namespace kernel;
    uint32_t mode = game::Opt().ambient_occlusion;
    // not in the front end ([0x95F8F8], null in a level): its one 3D thing is the turning badge beside the menus
    if (!mode || R32(kFrontEnd)) return;
    uint32_t scene = R32(kScene);
    X86HostFp host_fp;
    bool solid = scene && R32(scene + 0x300) == 1 && R32(scene + 0x30C);
    // Scenes with one solid node each follow the world's (far 8192 too): only the first of a run is shaded.
    static bool shaded = false;
    if (!solid || !(G<float>(scene + 0x3C8) > kWorldFar)) {
        shaded = false;
        // The gun's scene (it sees 128 units): its depth holds the gun and the hands alone, the world's cleared away,
        // so they shade themselves and nothing behind them. No fog is drawn on them.
        if (solid && G<float>(scene + 0x3C8) > kGunFar) {
            const float none[3] = {};
            d3d::Occlusion(&G<float>(scene + 0x10), mode == 2, kGunRadius, kStrength, none, none);
        }
        return;
    }
    if (shaded) return;
    shaded = true;
    float fog[3] = {}, colour[3] = {};
    if (G<uint8_t>(scene + 0x384)) {
        float from = G<float>(scene + 0x388), to = G<float>(scene + 0x38C);
        float least = G<float>(scene + 0x390), most = G<float>(scene + 0x394);
        if (to > from && most > least && std::isfinite(to - from)) {
            fog[0] = (most - least) / (to - from);
            fog[1] = least - from * fog[0];
            fog[2] = most;
            for (int i = 0; i < 3; i++) colour[i] = G<float>(scene + 0x3A0 + uint32_t(i) * 4);
        }
    }
    d3d::Occlusion(&G<float>(scene + 0x10), mode == 2, kRadius, kStrength, fog, colour);
}
