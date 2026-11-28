// Iron sights on the aim point.
//
// Aimed down the sights the gun is put exactly at the eye and along the view (CG_CalculateWeaponPosition 0x282D10
// leaves no offset, CG_CalculateWeaponAngles 0x285AF0 only the idle sway), so where its sights sit on the screen is
// the view model's own pose: and no gun's pose puts its front sight on the middle of the screen, where the shot
// goes. Each gun is off by its own amount, sideways and up or down (the M1 Carbine's post a third of a degree high,
// the MP40's as much low). It is the game's own, on the console as here: an angle, the same at every resolution and
// field of view. The console's sightFix 0 leaves them as the game has them.
//
// Each gun is turned back by the angles measured for it, by how far the player has aimed (the fraction at
// 0x9725D8). Measured in the harness: the gun aimed with its idle sway off before a plain background, magnified six
// times, the tip of the front sight (the notch for the Carcano, which shows no blade) against the middle of the
// frame; one degree of turn moves the sight 72 pixels across and 92 down there (measured by turning the gun a
// degree). To each 0.06 degrees left and 0.03 up are added: by that much a 2560 x 1440 recording of the M1 Carbine
// on Windows differed from the harness (with 0.18 and 0.23 its post stood 1.5 pixels left of the middle and 3.6 over
// it, at 46 pixels a degree). A gun not measured gets the usual turn left alone.
// The console's sightYaw and sightPitch add to the gun in hand's, to find better ones.

#include <cstring>

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

using kernel::G;
using kernel::R32;

constexpr uint32_t kWeaponDefs = 0x95F4D8;  // the loaded weapons' definitions, by index
constexpr uint32_t kWeaponInHand = 0x9725D0;
constexpr uint32_t kAimed = 0x9725D8;       // 0 from the hip to 1 down the sights

struct Sight {
    const char* weapon;  // the definition's name (+8)
    float yaw;           // degrees to the left
    float pitch;         // degrees down
    float blur = 0;      // where Blur Gun Sight ends, in near planes of the gun's scene (0: the usual)
};
constexpr float kUsual = 0.09f;  // for a gun not measured: the mean of those that are
constexpr Sight kSights[] = {
    // harness: 0.09 right, 0.34 high; set from the Windows recording
    {"m1carbine", 0.15f, 0.31f},
    {"m1garand", 0.09f, -0.04f},  // 0.03 right, 0.01 low
    {"m1garand_medic", 0.09f, -0.04f},
    {"m1garand_engineer", 0.09f, -0.04f},
    {"thompson", 0.05f, -0.10f},  // 0.01 left, 0.07 low
    {"bar", 0.11f, -0.11f},       // 0.05 right, 0.08 low
    {"mp40", 0.14f, -0.35f},      // 0.08 right, 0.32 low (the post in its hood)
    {"kar98", 0.19f, 0.26f},      // 0.13 right, 0.29 high
    {"gewehr43", 0.09f, -0.15f},  // 0.03 right, 0.12 low
    // all that shows of it aimed is its rear notch, 15 to 20 near planes away, nearer than where the blur usually ends
    {"carcano", 0.06f, 0, 15},    // the notch in the middle, its top on the middle line
    {"breda_lmg", -0.05f, -0.25f},  // 0.11 left, 0.22 low
};

float g_pitch = 0;  // with Yaw()'s answer: the gun in hand's turn down
float g_blur = 0;   // and where its blur ends

// the turn for the weapon in hand
float Yaw() {
    static uint32_t known = 0;
    static float yaw = 0;
    uint32_t defs = R32(kWeaponDefs);
    uint32_t def = defs ? R32(defs + R32(kWeaponInHand) * 4) : 0;
    if (def == known) return yaw;
    known = def;
    yaw = 0;
    g_pitch = 0;
    g_blur = 0;
    uint32_t name = def ? R32(def + 8) : 0;
    if (!name) return yaw;
    // a gun aimed through its own sights: one that aims (aimDownSight, +0x6DC) with no scope picture over the screen
    // (adsOverlayShader, +0x634). Every one of them sits to the right (the three measured: 0.18 to 0.28 degrees), so
    // one not measured yet gets what is usual
    uint32_t scope = R32(def + 0x634);
    bool scoped = scope >= 0x10000 && mem::IsCommitted(scope) && kernel::G<uint8_t>(scope) != 0;
    if (!R32(def + 0x6DC) || scoped) return yaw;
    yaw = kUsual;
    const char* text = static_cast<const char*>(kernel::HostPtr(name));
    for (const Sight& s : kSights) {
        if (strncmp(text, s.weapon, 32) == 0) {
            yaw = s.yaw;
            g_pitch = s.pitch;
            g_blur = s.blur;
        }
    }
    return yaw;
}

}  // namespace

namespace game {
// Where the gun in hand's blur ends for the renderer (gpu::g_gun_reach): a steady distance for each gun
float GunBlurEnd() {
    Yaw();
    return g_blur;
}
}  // namespace game

// CG_CalculateWeaponAngles: the view weapon's pitch, yaw and roll into eax
X86_EXTERN_FUNC(__imp__sub_00285AF0);
X86_FUNC(sub_00285AF0) {
    uint32_t angles = ctx.eax;
    __imp__sub_00285AF0(ctx, base);
    X86HostFp host_fp;
    float aimed = G<float>(kAimed);
    if (aimed > 0 && game::g_sight_fix) {
        G<float>(angles + 4) += aimed * (Yaw() + game::g_sight_yaw);
        G<float>(angles) += aimed * (g_pitch + game::g_sight_pitch);
    }
}
