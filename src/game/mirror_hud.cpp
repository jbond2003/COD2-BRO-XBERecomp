// The sdrawkcaB cheat's mirror: the HUD and every line of text on it drawn left to right reversed.
//
// The renderer writes its 2D pictures (0x78DBA to 0x78FE1) and its text (the string's draw 0x79FC0, 0x7A154 to
// 0x7A380) straight into the push buffer: a begin (0x17FC, 8: quads), inline vertices (0x1818) of six words each
// (x, y, z, colour, u, v, in the 640 by 480 the game lays its screens out in, a quad's corners going round from the
// top left) and an end. Between the game's begin and end of such a run the x of every vertex is turned round the
// middle of the screen, and each quad's second and fourth corners change places so it still faces the viewer.
// Only in play with the cheat on: the menus, the pause menu among them, stay as they are.

#include <cstdlib>
#include <cstring>

#include "common/cheats.h"
#include "common/mouse_keyboard.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr float kWidth = 640.0f;
constexpr uint32_t kVertexWords = 6, kQuadWords = kVertexWords * 4;

uint32_t g_start[2];

bool Mirrored() {
    static const bool test = std::getenv("COD2_MIRROR") != nullptr;  // the harness: the menus, to look at it
    if (test) return true;
    using namespace game::cheats;
    return Unlocked() && On(kBackwards) && !InMenus() && input::Playing();
}

void Mirror(uint32_t start, uint32_t end) {
    if (!start || end <= start || end - start > (1u << 20)) return;
    uint32_t words = (end - start) / 4;
    for (uint32_t i = 0; i < words;) {
        uint32_t head = kernel::R32(start + i * 4), count = head >> 18 & 0x7FF;
        if ((head & 0xFFFF) == 0x1818 && count % kQuadWords == 0 && i + 1 + count <= words) {
            for (uint32_t q = 0; q < count; q += kQuadWords) {
                uint32_t quad = start + (i + 1 + q) * 4;
                for (uint32_t v = 0; v < 4; v++) {
                    float& x = kernel::G<float>(quad + v * kVertexWords * 4);
                    x = kWidth - x;
                }
                void* second = kernel::HostPtr(quad + kVertexWords * 4);
                void* fourth = kernel::HostPtr(quad + 3 * kVertexWords * 4);
                uint8_t held[kVertexWords * 4];
                std::memcpy(held, second, sizeof(held));
                std::memcpy(second, fourth, sizeof(held));
                std::memcpy(fourth, held, sizeof(held));
            }
        }
        i += 1 + count;
    }
}

}  // namespace

// just after the renderer's BeginPush (eax the buffer), and at its EndPush (the end is the argument)
void MirrorBeginPicture(X86Context& ctx, uint8_t*) { g_start[0] = Mirrored() ? ctx.eax : 0; }
void MirrorEndPicture(X86Context& ctx, uint8_t*) { Mirror(g_start[0], kernel::R32(ctx.esp)), g_start[0] = 0; }
void MirrorBeginText(X86Context& ctx, uint8_t*) { g_start[1] = Mirrored() ? ctx.eax : 0; }
void MirrorEndText(X86Context& ctx, uint8_t*) { Mirror(g_start[1], kernel::R32(ctx.esp)), g_start[1] = 0; }
