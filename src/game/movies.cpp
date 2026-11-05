// Movies (.xbv) show one decoded frame per game frame; the original held those frames to two vertical blanks
// (nglSetVBlankMode 2 through the D3D vertical blank callback), which the host's pacing does not see. A frame that
// decodes a movie frame is held to 30 fps, so movies play at their speed while the game runs at 60.

#include "app/app.h"
#include "cpu/x86_ops.h"

X86_EXTERN_FUNC(__imp__sub_000CE210);

// Movie decode, slot 2 of the movie vtable 0x7CF390
X86_FUNC(sub_000CE210) {
    __imp__sub_000CE210(ctx, base);
    app::LimitNextFrame(30);
}
