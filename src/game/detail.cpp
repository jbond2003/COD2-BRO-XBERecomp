// How far away models keep their detail.
//
// A soldier's model has up to five levels of detail. G_UpdateEntityLOD (0x2B8450, each entity each game frame)
// takes the squared distance to the player times the zoom's scale ([0x8DA6D8]) and picks level 2 from 240 units,
// 3 from 480 and 4 from 720 ([0x83EE58..0x83EE60], squared). Scenery with levels of its own is picked by
// nglSelectMeshLod (0x68560): the view depth, at 0x6864F in xmm0, against the mesh's own distances. The View
// Distance setting (PC Options, Graphics) divides both distances, so the levels change that many times further
// away; at 1 the game's own are never touched.
//
// A body in the level it fell in. R_AddDObjSurfaces (0x244020) never draws a model finer than the level its pose
// was last worked out for (the pose's state at DObj +0x68, its level at +4: 0x244495..0x2444B7 make it the DObj's
// coarsest allowed, +0x98). A living soldier's pose is worked out again each frame, at the level in use. A body the
// physics moves is not (DObj_UpdateSkeletons 0x2DF880 leaves at once for entity flag 0x400000): its state keeps the
// level it had as the soldier fell, and a soldier shot far away stayed a few flat shapes when walked up to. The
// physics fills every bone of the finest level's list (DObj_CalcBasePose 0x240F00, XModel_CalcSkeleton over the
// first level the model has), so such a body is drawn at the level its distance asks for.

#include <algorithm>

#include "common/options.h"
#include "cpu/x86_ops.h"

namespace {

float Further() { return float(std::clamp(game::Opt().view_distance, 1u, 8u)); }

}  // namespace

// G_UpdateEntityLOD, xmm0 the squared distance about to be compared
void EntityLodDistance(X86Context& ctx, uint8_t*) {
    float k = Further();
    if (k > 1.0f) ctx.xmm[0].f[0] /= k * k;
}

// nglSelectMeshLod, xmm0 the depth about to be compared
void MeshLodDistance(X86Context& ctx, uint8_t*) {
    float k = Further();
    if (k > 1.0f) ctx.xmm[0].f[0] /= k;
}

// R_AddDObjSurfaces, eax the entity's flags (0 without one), about to be tested for 0x40000000 (no coarsest allowed
// level): a body the physics poses has none either
void BodyLodLimit(X86Context& ctx, uint8_t*) {
    if (ctx.eax & 0x400000) ctx.eax |= 0x40000000;
}
