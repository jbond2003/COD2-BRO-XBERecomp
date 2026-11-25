// What the game does once a frame, at frame rates it was not made for.
//
// A vehicle's wheels: Vehicle_UpdatePhysics (0x26A3C0) drops each wheel by gravity and, when the ground it finds is
// above the wheel, puts the wheel on it at once (0x26AFBD); the body is laid through the wheels, and the next
// frame's search for the ground starts from the body. A wheel against something tall (a wall, a tree, another
// vehicle) so climbs its whole reach, about 70 units, every frame it touches it: one or two frames at 30 a second,
// twenty at 300, where the prologue's first Sherman went a thousand units up and rolled to its limit. Here a wheel
// climbs at most a rate, a little over what one frame gave at 30 a second, whatever the frame rate.
//
// The player's push out of solids, and the view's creep in a vehicle, below.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

#include "common/menu_state.h"

namespace {

constexpr float kWheelRise = 2400.0f;  // units a second; the game's reach is about 70 a frame

}  // namespace

// Vehicle_UpdatePhysics (ebp+8: the entity, its vehicle at +0x248; ebp+0xC: moved by time, ebp+0x10: the frame's
// milliseconds), a wheel about to be put on the ground: eax its height, already dropped for the frame, xmm2 the
// ground's. It climbs at most the rate above the higher of its own height and the other wheels' (+0x138, 4 or 6 of
// them): a wheel against a wall is held, but one that has dropped below the others (over a hole, the edge of a
// crater: The Desert Fox's minefield) is back on the ground at once as before. Held to the rate from where it had
// dropped, it could not catch up, the body leaned through it, and the tank sank through the ground on its side.
void WheelRise(X86Context& ctx, uint8_t*) {
    if (!kernel::R32(ctx.ebp + 0xC)) return;  // placed, not driven: on the ground as found
    X86HostFp host_fp;
    auto read = [](uint32_t at) {
        uint32_t raw = kernel::R32(at);
        float v;
        std::memcpy(&v, &raw, 4);
        return v;
    };
    float height = read(ctx.eax), from = height;
    uint32_t entity = kernel::R32(ctx.ebp + 8), vehicle = entity ? kernel::R32(entity + 0x248) : 0;
    if (vehicle) {
        int16_t kind = int16_t(kernel::R32(vehicle + 0x178) & 0xFFFF);
        uint32_t def = kind >= 0 ? kernel::R32(0x950A28 + uint32_t(kind) * 4) : 0;
        uint32_t wheels = def && (kernel::R32(def + 0x20) & 0xFFFF) == 1 ? 4 : 6;
        uint32_t first = vehicle + 0x138;
        if (ctx.eax >= first && ctx.eax < first + wheels * 4)
            for (uint32_t i = 0; i < wheels; i++) {
                uint32_t at = first + i * 4;
                if (at != ctx.eax) from = std::max(from, read(at));
            }
    }
    float limit = from + kWheelRise * float(int32_t(kernel::R32(ctx.ebp + 0x10))) * 0.001f;
    if (ctx.xmm[2].f[0] > limit) ctx.xmm[2].f[0] = limit;
}

// G_SeparateFromSolids (0x2E6260), at the end of every step the player takes on the ground or in the air: the player
// is pushed away from what stands near and out of what it touches, by distances that do not know the frame's length.
// On a slope or beside a squadmate each push leaves a little behind, so a player standing still crept, faster the
// higher the frame rate (the first level's start: 0.026 units a second down at 215 frames a second, 0.008 with
// this). It runs when the move's time crosses a 33 ms mark, 30 times a second as on the console; the time is the
// command's, so the server's move and the drawn one skip the same steps.
//
// And one push still moves a standing player: up to 0.07 units, always toward the map's origin whatever the slope
// (The Desert Fox's dunes: a unit a second). A push shorter than an eighth of a unit is taken back while the player
// neither moves nor is asked to.
X86_EXTERN_FUNC(__imp__sub_002E6260);

X86_FUNC(sub_002E6260) {
    constexpr uint32_t kMove = 0x92EFF4, kMoveMs = 0x95FFA8, kEvery = 33;  // pm (its command's time at +4), pml.msec
    constexpr float kCreep = 0.125f;  // units: a push this short does not move a player who stands still
    uint32_t pm = kernel::R32(kMove);
    uint32_t time = pm ? kernel::R32(pm + 4) : 0, ms = kernel::R32(kMoveMs);
    if (pm && ms < kEvery && time >= ms && time / kEvery == (time - ms) / kEvery) {
        ctx.esp += 4;  // the return address
        return;
    }
    uint32_t state = pm ? kernel::R32(pm) : 0;
    // standing still: no forward or sideways in the command (a signed byte each at pm +0x18), no speed (+0x10)
    bool still = state && !(kernel::R32(pm + 0x18) & 0xFFFF) && !(kernel::R32(state + 0x10) << 1) &&
                 !(kernel::R32(state + 0x14) << 1) && !(kernel::R32(state + 0x18) << 1);
    uint32_t before[3] = {};
    if (still)
        for (uint32_t i = 0; i < 3; i++) before[i] = kernel::R32(state + i * 4);
    __imp__sub_002E6260(ctx, base);
    if (!still) return;
    float moved = 0.0f;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t raw = kernel::R32(state + i * 4);
        float a, b;
        std::memcpy(&a, &before[i], 4);
        std::memcpy(&b, &raw, 4);
        moved += (b - a) * (b - a);
    }
    if (moved == 0.0f || moved >= kCreep * kCreep) return;
    for (uint32_t i = 0; i < 3; i++) kernel::W32(state + i * 4, before[i]);
}

// CL_FinishMove (0x2ABB30) turns each view angle into the command's 16 bit one as the whole part of angle * 65536 /
// 360 + 0.5, cut toward zero: right for an angle above zero, one step (0.0055 degrees) short for one below it. On
// foot that is a still offset nobody sees. In a vehicle the view is written back from the command's angles every
// frame (CG_CalcVehicleViewAngles, 0x2864A0), so a view angle below zero lost a step a frame: the tank's turret
// turned by itself, 0.16 degrees a second at 30 frames a second and 0.8 at 144. Here the angle below zero is
// brought down to its whole number first, so it comes out as the nearest step like the others.
// xmm2: the angle in steps with the half added, about to be cut
void ViewAngleStep(X86Context& ctx, uint8_t*) {
    X86HostFp host_fp;
    float steps = ctx.xmm[2].f[0];
    if (steps < 0.0f) ctx.xmm[2].f[0] = std::floor(steps);
}

// A report from The Desert Fox at 144 frames a second (the engineers at the minefield): a tank went forward, jumped
// back, and then showed lying on its side, flashing. Not seen here yet, so the log keeps what a vehicle did when it
// happens: every vehicle of the pool (g_vehicles, [0x95F694], 0x340 each, the count a word at 0x986000, in use with
// an entity at +0x170) is looked at once a frame, and one whose place moved more than a tank can in a frame, or
// that leans more than 50 degrees, is written with its place (+0x14), angles (+0xE0), physics place (+0xC0) and
// wheel heights (+0x138), before and now.
namespace game {

void WatchVehicles() {
    constexpr uint32_t kPool = 0x95F694, kCount = 0x986000, kSize = 0x340, kSlots = 32;
    constexpr float kLean = 50.0f;
    struct Seen {
        uint32_t entity = 0;
        float f[16] = {};  // place 3, angles 3, physics place 3, wheels 6, spare
        int64_t logged_ms = 0;
    };
    static Seen seen[kSlots];
    static int lines = 0;
    static float jump = 0.0f;
    if (jump == 0.0f) {
        const char* env = std::getenv("COD2BRO_TANK_JUMP");
        jump = env ? float(std::atof(env)) : 48.0f;
        if (!(jump > 0.0f)) jump = 48.0f;
    }
    uint32_t pool = kernel::R32(kPool), count = kernel::R32(kCount) & 0xFFFF;
    if (!pool || lines >= 600) return;
    if (count > kSlots) count = kSlots;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    auto read = [](uint32_t at) {
        uint32_t raw = kernel::R32(at);
        float v;
        std::memcpy(&v, &raw, 4);
        return v;
    };
    auto lean = [](float a) {
        a = std::fmod(a, 360.0f);
        if (a > 180.0f) a -= 360.0f;
        if (a < -180.0f) a += 360.0f;
        return std::fabs(a);
    };
    for (uint32_t i = 0; i < count; i++) {
        uint32_t v = pool + i * kSize, entity = kernel::R32(v + 0x170);
        Seen& was = seen[i];
        if (!entity) {
            was.entity = 0;
            continue;
        }
        float f[16] = {};
        for (uint32_t k = 0; k < 3; k++) f[k] = read(v + 0x14 + k * 4);
        for (uint32_t k = 0; k < 3; k++) f[3 + k] = read(v + 0xE0 + k * 4);
        for (uint32_t k = 0; k < 3; k++) f[6 + k] = read(v + 0xC0 + k * 4);
        for (uint32_t k = 0; k < 6; k++) f[9 + k] = read(v + 0x138 + k * 4);
        if (was.entity == entity) {
            float dx = f[0] - was.f[0], dy = f[1] - was.f[1], dz = f[2] - was.f[2];
            float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            bool placed = was.f[0] == 0.0f && was.f[1] == 0.0f && was.f[2] == 0.0f;  // just put in the map
            bool jumped = moved > jump && !placed, wheels = f[9] != 0.0f || f[10] != 0.0f,  // an aeroplane has none, and banks
                 down = wheels && (lean(f[3]) > kLean || lean(f[5]) > kLean);
            if (jumped || (down && now - was.logged_ms > 500)) {
                was.logged_ms = now;
                lines++;
                LOGR("vehicle %u %s: moved %.1f, at %.1f %.1f %.1f angles %.1f %.1f %.1f physics %.1f %.1f %.1f "
                     "wheels %.1f %.1f %.1f %.1f %.1f %.1f",
                     i, jumped ? "jumped" : "leans", moved, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8],
                     f[9], f[10], f[11], f[12], f[13], f[14]);
                LOGR("vehicle %u before: at %.1f %.1f %.1f angles %.1f %.1f %.1f physics %.1f %.1f %.1f "
                     "wheels %.1f %.1f %.1f %.1f %.1f %.1f",
                     i, was.f[0], was.f[1], was.f[2], was.f[3], was.f[4], was.f[5], was.f[6], was.f[7], was.f[8],
                     was.f[9], was.f[10], was.f[11], was.f[12], was.f[13], was.f[14]);
            }
        }
        was.entity = entity;
        std::memcpy(was.f, f, sizeof f);
    }
}

}  // namespace game
