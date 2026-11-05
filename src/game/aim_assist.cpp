// No aim assist on the keyboard and mouse. The view angle update (PM_UpdateViewAngles, 0x2E5530) calls the sticky
// aim at 0x2DB7D0 each frame (thiscall, playerState in ecx, plain ret): it finds an enemy under the crosshair, keeps it
// at ps+0x500 (+0x504, and the point on it at +0x508) and turns the view to follow it. While the keyboard and mouse
// are in use it does only what it does with no target there: clear +0x500. A controller keeps it. (Address and field
// from CoD2-BRO-Native-Loader's notes.)

#include "common/mouse_keyboard.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_002DB7D0);

X86_FUNC(sub_002DB7D0) {
    if (!input::KeyboardActive()) {
        __imp__sub_002DB7D0(ctx, base);
        return;
    }
    kernel::W32(ctx.ecx + 0x500, 0);
    ctx.esp += 4;  // its ret
}
