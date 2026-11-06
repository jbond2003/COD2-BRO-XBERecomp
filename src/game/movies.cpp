// Movies (.xbv) show one decoded frame per game frame; the original held those frames to two vertical blanks
// (nglSetVBlankMode 2 through the D3D vertical blank callback), which the host's pacing does not see. A frame that
// decodes a movie frame is held to 30 fps, so movies play at their speed while the game runs at 60: cinematics, and
// the menus with a moving background (the main menu). A decode cannot be skipped to keep those menus at 60: the movie
// texture shares memory the frame between overwrites, and a decode only redraws what changed.

#include "app/app.h"
#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(__imp__sub_000CE210);

// Movie decode (ecx movie), slot 2 of the movie vtable 0x7CF390
X86_FUNC(sub_000CE210) {
    __imp__sub_000CE210(ctx, base);
    app::LimitNextFrame(30);
}

// The movie loop (0x332230) ends a movie when 0x316010 says a pad button was pressed, once the movie's shortest time
// is up ([0x95F460] over the seconds shown [0x95F45C]; the legal screen has one, the logos and cinematics none). A left
// click, Enter, Space or Escape skips it the same way.
#include "common/mouse_keyboard.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00316010);

X86_FUNC(sub_00316010) {
    __imp__sub_00316010(ctx, base);
    static bool held = true;  // a press already down when the movie starts waits for its release
    bool now = input::SkipHeld();
    bool pressed = now && !held;
    held = now;
    if (!pressed || (ctx.eax & 0xFF)) return;
    if (kernel::G<float>(0x95F460) > kernel::G<float>(0x95F45C)) return;
    ctx.eax = (ctx.eax & ~0xFFu) | 1;
}
