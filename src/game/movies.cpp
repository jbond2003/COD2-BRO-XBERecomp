// Movies (.xbv) show one decoded frame per game frame; the original held those frames to two vertical blanks
// (nglSetVBlankMode 2 through the D3D vertical blank callback), which the host's pacing does not see.
// - Full screen movies (cinematics, logos: the movie loop 0x332230) hold the frame that decodes one to 30 fps, so they
//   play at their speed.
// - A movie behind a menu decodes at most 30 frames a second and the menu itself runs at the game's rate (60): a
//   frame in between keeps the last picture (the decode returns 1, done, without decoding).

#include <chrono>

#include "app/app.h"
#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(__imp__sub_000CE210);
X86_EXTERN_FUNC(__imp__sub_00332230);

namespace {
int g_movie_loop = 0;  // inside the full screen movie loop
}

// the full screen movie loop
X86_FUNC(sub_00332230) {
    g_movie_loop++;
    __imp__sub_00332230(ctx, base);
    g_movie_loop--;
}

// Movie decode (ecx movie), slot 2 of the movie vtable 0x7CF390, a plain ret
X86_FUNC(sub_000CE210) {
    if (g_movie_loop) {
        __imp__sub_000CE210(ctx, base);
        app::LimitNextFrame(30);
        return;
    }
    using clock = std::chrono::steady_clock;
    static uint32_t movie = 0;
    static clock::time_point next{};
    clock::time_point now = clock::now();
    if (ctx.ecx == movie && now < next) {
        ctx.eax = 1;
        ctx.esp += 4;  // its ret
        return;
    }
    movie = ctx.ecx;
    next = (now - next < std::chrono::milliseconds(100) ? next : now) + std::chrono::microseconds(33333);
    __imp__sub_000CE210(ctx, base);
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
