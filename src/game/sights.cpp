// Iron sights on the aim point.
//
// Aimed down the sights the gun is put exactly at the eye and along the view (CG_CalculateWeaponPosition 0x282D10
// leaves no offset, CG_CalculateWeaponAngles 0x285AF0 only the idle sway), so where its sights sit on the screen is
// the view model's own pose: and that pose leaves them a little to the right of the middle of the screen,
// where the shot goes. It is the game's own, on the console as here: an angle, the same at every resolution.
// The console's sightFix 0 leaves them as the game has them.
//
// The angles: measured in the harness against the middle of its frame, then all raised by 0.11 degrees, by which a
// Windows screenshot of the M1 Carbine showed them short (its post 11.3 of 2557 pixels right of the middle untouched,
// 7.0 with 0.07 degrees: 61 pixels a degree). The console's sightYaw and sightPitch add to the gun in hand's, to find better ones.
//
// Each gun is turned back by the angle measured for it (magnified 24 times in the harness, the front sight against
// the middle of the frame), or by the usual one where it has not been measured, by how far the player has aimed (the
// fraction at 0x9725D8).

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
};
constexpr float kUsual = 0.21f;  // for a gun not measured
constexpr Sight kSights[] = {
    {"m1carbine", 0.18f, 0.23f},  // the post's tip was 14 of 1437 pixels over the middle in the same screenshot
    {"carcano", 0.28f, 0},
    {"breda_lmg", 0.19f, 0},
};

float g_pitch = 0;  // with Yaw()'s answer: the gun in hand's turn down

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
        }
    }
    return yaw;
}

}  // namespace

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
