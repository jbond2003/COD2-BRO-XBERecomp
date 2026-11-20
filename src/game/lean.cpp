// Leaning held or toggled (the Gameplay setting "Leaning"; the game's own is held).
//
// The lean commands press and release two of the client's buttons, Quake III kbuttons (down[2] the keys holding it,
// downtime, msec, active at +0x10, wasPressed): the left lean's at 0x934140, the right's at 0x934158. +leanleft
// (0x2B4680) and +leanright (0x2B4660) are IN_KeyDown (0x2ABFE0) on them, -leanleft (0x2B4670) and -leanright
// (0x2B4650) IN_KeyUp (0x2ABF50). The default layout's "lean, or the next weapon when not aiming" pair
// (+leanleftswitchnext 0x2DB640, +leanrightswitchnext 0x2DB660) press the same buttons while [0x9601AC] has 0x20
// and release them with 0x2BB8F0 and 0x2BB900. Every way to lean ends in these: a controller's binding, and the
// keyboard's lean keys (which press the dpad or send the command, src/game/mouse_look.cpp).
//
// Toggled: a press leans and the release does nothing; the next press of the same side straightens, and a press of
// the other side leans that way instead. What is leaning is read from the buttons themselves, so a new level or a
// change of the setting cannot leave it out of step: with the setting back on hold, the next release straightens.

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kLeanLeft = 0x934140, kLeanRight = 0x934158;
constexpr uint32_t kAimFlags = 0x9601AC;  // 0x20: the switchnext pair leans (else it changes the weapon)

bool Toggled() { return game::Opt().lean_toggle; }
bool Down(uint32_t button) { return kernel::R32(button + 0x10) != 0; }
// as IN_KeyUp does for a release typed at the console: no key holds it, and it is not active
void Release(uint32_t button) {
    kernel::W32(button, 0);
    kernel::W32(button + 4, 0);
    kernel::W32(button + 0x10, 0);
}

// A side's key is still held from its last press (0 left, 1 right): a held key may repeat its press, and a repeat
// must not toggle again. Cleared by the side's release command.
bool g_held[2];

// a press of a side's button while toggled: true when the game's press is left out (it straightened, or the press
// is a held key repeating)
bool Straightened(uint32_t button, uint32_t other) {
    bool& held = g_held[button == kLeanRight];
    if (held) return true;
    held = true;
    if (Down(button)) {
        Release(button);
        return true;
    }
    Release(other);
    return false;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002B4680);
X86_EXTERN_FUNC(__imp__sub_002B4670);
X86_EXTERN_FUNC(__imp__sub_002B4660);
X86_EXTERN_FUNC(__imp__sub_002B4650);
X86_EXTERN_FUNC(__imp__sub_002DB640);
X86_EXTERN_FUNC(__imp__sub_002DB660);
X86_EXTERN_FUNC(__imp__sub_002BB8F0);
X86_EXTERN_FUNC(__imp__sub_002BB900);

// +leanleft, +leanright
X86_FUNC(sub_002B4680) {
    if (Toggled() && Straightened(kLeanLeft, kLeanRight)) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002B4680(ctx, base);
}
X86_FUNC(sub_002B4660) {
    if (Toggled() && Straightened(kLeanRight, kLeanLeft)) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002B4660(ctx, base);
}
// -leanleft, -leanright
X86_FUNC(sub_002B4670) {
    g_held[0] = false;
    if (Toggled()) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002B4670(ctx, base);
}
X86_FUNC(sub_002B4650) {
    g_held[1] = false;
    if (Toggled()) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002B4650(ctx, base);
}
// +leanleftswitchnext, +leanrightswitchnext: toggled only when they lean
X86_FUNC(sub_002DB640) {
    if (Toggled() && (kernel::R32(kAimFlags) & 0x20) && Straightened(kLeanLeft, kLeanRight))
        return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002DB640(ctx, base);
}
X86_FUNC(sub_002DB660) {
    if (Toggled() && (kernel::R32(kAimFlags) & 0x20) && Straightened(kLeanRight, kLeanLeft))
        return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002DB660(ctx, base);
}
// -leanleftswitchnext, -leanrightswitchnext
X86_FUNC(sub_002BB8F0) {
    g_held[0] = false;
    if (Toggled()) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002BB8F0(ctx, base);
}
X86_FUNC(sub_002BB900) {
    g_held[1] = false;
    if (Toggled()) return kernel::Return(ctx, ctx.eax, 0);
    __imp__sub_002BB900(ctx, base);
}
