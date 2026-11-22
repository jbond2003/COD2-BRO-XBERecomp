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
