// Movies (.xbv) show one decoded frame per game frame; the original held those frames to two vertical blanks
// (nglSetVBlankMode 2 through the D3D vertical blank callback), which the host's pacing does not see.
// - Full screen movies (logos, cinematics: the movie loop 0x332230) hold the frame that decodes one to 30 fps, so they
//   play at their speed.
// - A movie behind a menu (the main menu's, drawn at 0x31B62D) decodes 30 times a second while the menu runs at 60.
//   With +0x74 set the decode asks 0x6AAE0 for its target, the engine texture 0x902D50 over the back buffer (+0x1C
//   pitch, +0x20 bits, +0x30 D3D surface), locks it and writes every pixel with alpha 0. The picture is on screen
//   because it was written into the back buffer itself: the menu's quad of that texture blends it away. So a frame
//   without a decode showed whatever that buffer last held (old screens, a stuttering shadow), and the lock read the
//   frame back from the GPU, stalling every frame that decoded. Here the decode gets textures of our own instead,
//   copies of 0x902D50 over our own memory, two in turn like the console's two buffers, made opaque after it: the
//   quad draws the picture on every frame, the frames in between draw the last one, and nothing is read back.

#include <algorithm>
#include <chrono>
#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

X86_EXTERN_FUNC(__imp__sub_000CE210);
X86_EXTERN_FUNC(__imp__sub_00332230);
X86_EXTERN_FUNC(__imp__sub_0006AAE0);

namespace {
constexpr uint32_t kBackBufferTexture = 0x902D50;  // engine texture: +0x1C pitch, +0x20 bits, +0x30 D3D surface
constexpr uint32_t kTextureBytes = 0x78;           // up to the next engine object (0x902DC8)
constexpr uint32_t kBlockUntilNotBusy = 0x3E8000;  // D3DResource_BlockUntilNotBusy (resource), stdcall
int g_movie_loop = 0;                              // inside the full screen movie loop
bool g_menu_decode = false;                        // inside the decode of a movie behind a menu

// a texture of our own the shape of 0x902D50: the engine texture, its D3D surface, the picture's memory
struct Picture {
    uint32_t tex = 0, surface = 0;
};
Picture g_pics[2];
uint32_t g_bytes = 0;
int g_turn = 0;

// the bytes of an engine texture's picture, from its D3D surface's Size: height - 1 at bits 12..23, pitch / 64 - 1
// from bit 24 (the engine texture's own +0x10 is not a size)
uint32_t PictureBytes(uint32_t tex) {
    uint32_t surface = kernel::R32(tex + 0x30);
    if (!surface) return 0;
    uint32_t size = kernel::R32(surface + 0x10);
    return ((size >> 12 & 0xFFF) + 1) * ((size >> 24) + 1) * 64;
}

Picture MakeOne(uint32_t bytes) {
    uint32_t surface = kernel::R32(kBackBufferTexture + 0x30);
    uint32_t bits = mem::AllocContiguous(bytes, 0, mem::kPhysicalSize - 1, mem::kPage);
    if (!bits) return {};
    std::memset(kernel::HostPtr(bits), 0, bytes);
    uint32_t tex = mem::AllocSystem(kTextureBytes + 4), own = mem::AllocSystem(20);
    std::memcpy(kernel::HostPtr(tex), kernel::HostPtr(kBackBufferTexture), kTextureBytes);
    std::memcpy(kernel::HostPtr(own), kernel::HostPtr(surface), 20);  // D3D: Common, Data, Lock, Format, Size
    kernel::W32(own, (kernel::R32(surface) & ~0xFFFFu) | 1);           // one reference, ours
    kernel::W32(own + 4, mem::PhysicalFromVirtual(bits));
    kernel::W32(own + 8, 0);
    kernel::W32(tex + 0x20, bits);
    kernel::W32(tex + 0x30, own);
    kernel::W32(tex + kTextureBytes, own);  // the surface cell +0x34 points at
    kernel::W32(tex + 0x34, tex + kTextureBytes);
    return {tex, own};
}

// made once, the shape of the back buffer texture; false when there is no memory for them
bool MakePictures() {
    uint32_t bytes = PictureBytes(kBackBufferTexture);
    if (g_pics[1].tex) return g_bytes == bytes;
    if (!bytes) return false;
    for (Picture& p : g_pics)
        if (!p.tex) p = MakeOne(bytes);
    if (!g_pics[1].tex) {
        static bool told = false;
        if (!told) LOGR("movies: no memory for the menu movie's pictures, menus with one stay at 30 fps");
        told = true;
        return false;
    }
    g_bytes = bytes;
    return true;
}
}  // namespace

// the full screen movie loop
X86_FUNC(sub_00332230) {
    g_movie_loop++;
    __imp__sub_00332230(ctx, base);
    g_movie_loop--;
}

// () -> the engine texture over the current back buffer, the decode's target with +0x74 set: ours in turn instead for
// a movie behind a menu, once the GPU is done with it
X86_FUNC(sub_0006AAE0) {
    if (!g_menu_decode) return __imp__sub_0006AAE0(ctx, base);
    const Picture& p = g_pics[g_turn];
    g_turn ^= 1;
    kernel::CallGuest(ctx, kBlockUntilNotBusy, {p.surface});
    ctx.eax = p.tex;
    ctx.esp += 4;  // its ret
}

// Movie decode (ecx movie), slot 2 of the movie vtable 0x7CF390, a plain ret; the callers draw [+0x30] after it, which
// the decode leaves on the texture it wrote
X86_FUNC(sub_000CE210) {
    uint32_t movie = ctx.ecx;
    bool behind_menu = !g_movie_loop && (kernel::R32(movie + 0x74) & 0xFF);
    if (!behind_menu || !MakePictures()) {
        __imp__sub_000CE210(ctx, base);
        // a full screen movie, one in a texture of its own, or no memory for ours: the frame waits for the movie as
        // the original did
        app::LimitNextFrame(30);
        return;
    }
    // time owed to the movie, a decode each 1/30 s; one is due half a 60 Hz frame early, so frames that come a little
    // unevenly still decode every other one
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    constexpr double kPeriod = 1000.0 / 30, kEarly = 1000.0 / 120;
    static uint32_t last = 0;
    static clock::time_point prev{};
    static double owed = 0;
    clock::time_point now = clock::now();
    owed = std::min(owed + ms(now - prev).count(), 100.0);
    prev = now;
    // A menu movie's picture that is not ours (just opened, or left on the engine's back buffer texture) decodes now:
    // drawn, that texture is the back buffer's memory, the last picture a full screen movie left there (a frame of the
    // intro, full screen, for a frame going from the profile screen to the main menu)
    uint32_t shown = kernel::R32(movie + 0x30);
    bool ours = shown == g_pics[0].tex || shown == g_pics[1].tex;
    if (movie == last && ours && owed < kPeriod - kEarly) {
        ctx.eax = 0;
        ctx.esp += 4;  // its ret
        return;
    }
    owed = movie == last ? owed - kPeriod : 0;
    last = movie;
    // (a movie's first decode decodes again inside itself: the outer one keeps our target after the inner one ends)
    bool outer = g_menu_decode;
    g_menu_decode = true;
    __imp__sub_000CE210(ctx, base);
    g_menu_decode = outer;
    // the decode leaves alpha 0, which the menu's quad blends away: opaque, so the quad shows the picture
    uint32_t tex = kernel::R32(movie + 0x30);
    if (ctx.eax == 0 && (tex == g_pics[0].tex || tex == g_pics[1].tex)) {
        uint32_t phys = kernel::R32(kernel::R32(tex + 0x30) + 4);
        uint8_t* p = static_cast<uint8_t*>(kernel::HostPtr(mem::VirtualFromPhysical(phys)));
        for (uint32_t i = 3; i < g_bytes; i += 4) p[i] = 0xFF;
    }
}

// The movie loop (0x332230) ends a movie when 0x316010 says a pad button was pressed, once the movie's shortest time
// is up ([0x95F460] over the seconds shown [0x95F45C]; the legal screen has one, the logos and cinematics none). A left
// click, Enter, Space or Escape skips it the same way.
#include "common/mouse_keyboard.h"

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
