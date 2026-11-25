// Iron sights on the aim point.
//
// Aimed down the sights the gun is put exactly at the eye and along the view (CG_CalculateWeaponPosition 0x282D10
// leaves no offset, CG_CalculateWeaponAngles 0x285AF0 only the idle sway), so where its sights sit on the screen is
// the view model's own pose: and on some guns that pose leaves them a little to the right of the middle of the screen,
// where the shot goes. At the Xbox's 480 lines it is one or two pixels; at a PC's resolution it shows.
// The console's sightFix 0 leaves them as the game has them.
//
// Each such gun is turned back by the angle measured for it (magnified 24 times in the harness, the front sight
// against the middle of the frame), by how far the player has aimed (the fraction at 0x9725D8).

#include <cstring>

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

using kernel::G;
using kernel::R32;

constexpr uint32_t kWeaponDefs = 0x95F4D8;  // the loaded weapons' definitions, by index
constexpr uint32_t kWeaponInHand = 0x9725D0;
constexpr uint32_t kAimed = 0x9725D8;       // 0 from the hip to 1 down the sights

struct Sight {
    const char* weapon;  // the definition's name (+8)
    float yaw;           // degrees to the left
};
constexpr Sight kSights[] = {
    {"m1carbine", 0.07f},
    {"carcano", 0.17f},
    {"breda_lmg", 0.08f},
};

// the turn for the weapon in hand
float Yaw() {
    static uint32_t known = 0;
    static float yaw = 0;
    uint32_t defs = R32(kWeaponDefs);
    uint32_t def = defs ? R32(defs + R32(kWeaponInHand) * 4) : 0;
    if (def == known) return yaw;
    known = def;
    yaw = 0;
    uint32_t name = def ? R32(def + 8) : 0;
    if (!name) return yaw;
    const char* text = static_cast<const char*>(kernel::HostPtr(name));
    for (const Sight& s : kSights) {
        if (strncmp(text, s.weapon, 32) == 0) yaw = s.yaw;
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
    if (aimed > 0 && game::g_sight_fix) G<float>(angles + 4) += aimed * Yaw();
}
