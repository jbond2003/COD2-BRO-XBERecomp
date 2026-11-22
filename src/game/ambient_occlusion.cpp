// Ambient occlusion (Graphics, off unless chosen): soft shade where surfaces meet, worked out by the renderer from
// the depth of what is drawn.
//
// The game draws through NGL scenes: nglRenderSceneTree (0x74330) draws the current scene's ([0x90259C]) solid list
// (the call at 0x743F6), then its translucent one (0x7446D): smoke, glass, the glow. The shade goes in between, so
// it lies on the world, the soldiers and the vehicles and under everything see-through; the gun's own scene and the
// HUD come later still. The world's scene is the one in perspective with the many solid nodes (the sky's has none,
// the gun's a few, a shadow's is drawn to a small target, which the renderer leaves).
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
constexpr uint32_t kWorldNodes = 25;  // fewer solid nodes is not the world's scene
constexpr float kRadius = 28.0f;      // how far a surface is shaded from: the game's units are about an inch
constexpr float kStrength = 1.4f;     // 30% lighter than 2.0 (his ask): 1.4x the shade the shaders work out

}  // namespace

// nglRenderSceneTree, the scene's solid list just drawn
void AmbientOcclusion(X86Context&, uint8_t*) {
    using namespace kernel;
    uint32_t mode = game::Opt().ambient_occlusion;
    if (!mode) return;
    uint32_t scene = R32(kScene);
    if (!scene || R32(scene + 0x300) != 1 || R32(scene + 0x30C) < kWorldNodes) return;
    X86HostFp host_fp;
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
