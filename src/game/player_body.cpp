// Your own body, seen when you look down. Always on: there is no setting for it.
//
// The game draws no body for the player: only the gun and the arms that hold it. A script model
// is spawned (GScr_Spawn 0x3061E0, as the level scripts spawn theirs) with the body model of one of the level's own
// 1st Infantry soldiers (an "actor_<level>_1stInf..." entity's model, +0x258) and the soldiers' animation tree
// ([0x987A90], G_SetEntityAnimTree 0x25DF60), so the game animates, lights and draws it as it does any soldier. It
// is not solid (contents +0x134 are 0): nothing collides with it and no shot hits it.
//
// R_AddEntityModel (0x245A20, eax the entity as the renderer has it: +0x94 its axis, +0xBC its origin, +0xD4 its
// DObj, +0xD8 the game's entity) is where it is put under the camera each frame: at the player's feet, a little
// behind the eye, turned as the view is. There too the neck and all that hangs from it (head, helmet, arms, hands)
// and the pack are drawn into one point at the neck: the view is from the head, and the gun's own scene has the arms.
// It stands or runs (stand_alertb_idle1, combatrun_forward_1), crouches (crouch_alert_a_idle,
// crouchrun_loop_forward_1) by the eye's height and the player's speed over the ground; lying down it is not drawn.
// It is not drawn either while the camera is away from the player's head (a cutscene) or the player is dead.
//
// A checkpoint restarts the map (SV_MapRestart), so the body is never in a save: the level's clock running back
// drops it and it is spawned again.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00245A20);

namespace {
using namespace kernel;

constexpr uint32_t kEntities = 0x97A968, kEntityCount = 0x540;  // 8 bytes each: the entity, its generation
constexpr uint32_t kScene = 0x90259C;                           // NGL's current scene: +0x250 camera, +0x260 forward
constexpr uint32_t kLevelTime = 0x98540C;
constexpr uint32_t kActorAnims = 0x987A90;
constexpr uint32_t kSpawn = 0x3061E0;        // GScr_Spawn (class name slot, origin, 0) -> handle, cdecl
constexpr uint32_t kSetAnimTree = 0x25DF60;  // G_SetEntityAnimTree: eax the anims, edi the entity
constexpr uint32_t kSetAnimKnobAll = 0x3019B0;  // GScr_SetAnimKnobAll (handle, anim, root, weight, blend, rate, restart)
// the soldiers' tree (generic_human), by its nodes' numbers
constexpr uint32_t kStand = 165, kRun = 242, kCrouch = 135, kCrouchRun = 234;
constexpr float kBehind = 11.0f;  // units behind the eye: looking down shows the chest, the legs and the boots
// The hidden parts are drawn into one point inside the chest, below the camera's near plane (6 units): any higher
// and the plane cuts the cloth open. The cloth stretched to that point is not a pretty collar, so the body stands
// far enough behind the eye that the view's bottom edge stops short of it (kReach, kLean).
constexpr float kSunk = 6.0f;
constexpr float kReach = 20.0f;  // times the tangent of half the view's height: 18 units at a field of view of 110
constexpr float kLean = 4.0f;    // further back at a run, which leans the chest forward
constexpr float kFarBehind = 24.0f, kStanding = 60.0f, kCrouching = 40.0f;  // the eye above the feet
constexpr float kMoving = 40.0f;  // units a second over the ground

struct {
    uint32_t handle = 0, anim = 0, last_time = 0, next_try = 0;
    uint32_t model = 0, bones = 0;  // the model the hidden bones were worked out for
    int neck = -1;
    bool hide[128] = {};
    float x = 0, y = 0, speed = 0;
    float behind = 0, height = 0;  // eased: where the body stands behind the eye, how far below it the feet are
    std::chrono::steady_clock::time_point at, drawn;
} g;

uint32_t Player() {
    for (uint32_t i = 0; i < kEntityCount; i++) {
        uint32_t ent = R32(kEntities + i * 8);
        if (ent && R32(ent + 0x23C)) return ent;
    }
    return 0;
}

const char* ClassName(uint32_t ent) {
    uint32_t name = R32(ent + 0x25C);
    return name ? &G<char>(name + 6) : "";
}

bool Has(const char* text, const char* part) {
    size_t n = std::strlen(part);
    for (; *text; text++) {
        size_t i = 0;
        while (i < n && text[i] && (text[i] | 0x20) == (part[i] | 0x20)) i++;
        if (i == n) return true;
    }
    return false;
}

// the body's entity, 0 when it is gone (the level ended or restarted, a script deleted it)
uint32_t Body() {
    if (!g.handle) return 0;
    uint32_t i = g.handle & 0xFFF;
    uint32_t ent = i < kEntityCount ? R32(kEntities + i * 8) : 0;
    if (!ent || R32(kEntities + i * 8 + 4) != g.handle >> 12 || G<uint8_t>(ent) == 11 || !R32(ent + 0x254) ||
        R32(R32(ent + 0x254) + 8) != R32(kActorAnims) || std::strcmp(ClassName(ent), "script_model")) {
        g.handle = 0;
        return 0;
    }
    return ent;
}

// a call into the game from the hook: the registers and the hook's own return address are as they were after
struct Kept {
    X86Context& ctx;
    uint32_t back, r[7];
    explicit Kept(X86Context& c) : ctx(c), back(R32(c.esp)), r{c.eax, c.ecx, c.edx, c.ebx, c.esi, c.edi, c.ebp} {}
    ~Kept() {
        ctx.eax = r[0], ctx.ecx = r[1], ctx.edx = r[2], ctx.ebx = r[3], ctx.esi = r[4], ctx.edi = r[5], ctx.ebp = r[6];
        W32(ctx.esp, back);
    }
};

uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

void Play(X86Context& ctx, uint32_t anim, float blend) {
    if (anim == g.anim) return;
    g.anim = anim;
    Kept kept(ctx);
    CallGuest(ctx, kSetAnimKnobAll, {g.handle, anim, 0, Bits(1.0f), Bits(blend), Bits(1.0f), 1});
}

// soldier: one of the level's 1st Infantry men, whose body model it takes
bool Spawn(X86Context& ctx, uint32_t soldier, uint32_t player) {
    uint32_t slot = 0;  // any script model's class name
    for (uint32_t i = 0; i < kEntityCount && !slot; i++) {
        uint32_t ent = R32(kEntities + i * 8);
        if (ent && !std::strcmp(ClassName(ent), "script_model")) slot = ent + 0x25C;
    }
    static int failed = 0;
    if (!slot || !R32(kActorAnims)) {
        if (failed++ < 5) LOGR("player body: not made yet (%s)", slot ? "no soldier animations" : "no script model in the level");
        return false;
    }
    Kept kept(ctx);
    uint32_t handle = CallGuest(ctx, kSpawn, {slot, player + 0x140, 0});
    uint32_t i = handle & 0xFFF;
    uint32_t ent = handle && i < kEntityCount ? R32(kEntities + i * 8) : 0;
    if (!ent) {
        if (failed++ < 5) LOGR("player body: not made yet (the game gave no entity)");
        return false;
    }
    W32(ent + 0x258, R32(soldier + 0x258));
    ctx.eax = R32(kActorAnims), ctx.edi = ent;
    CallGuest(ctx, kSetAnimTree, {});
    W32(ent + 0x134, 0);
    g.handle = handle, g.anim = 0, g.model = 0;
    static int said = 0;
    if (said++ < 20) LOGR("player body: spawned with the model of %s", ClassName(soldier));
    return true;
}

// which of the model's bones are drawn into the neck: it, what hangs from it, and the pack
void FindBones(uint32_t model) {
    g.model = model, g.neck = -1, g.bones = 0;
    std::memset(g.hide, 0, sizeof(g.hide));
    uint32_t lod = 0;
    for (uint32_t i = 0; i < 5 && !lod; i++) lod = R32(model + 0x24 + i * 4);
    uint32_t parts = lod ? R32(lod + 8) : 0;
    uint32_t count = parts ? R32(parts + 0x10) : 0, bones = parts ? R32(parts + 0x14) : 0;
    if (!bones || count > 128) return;
    for (uint32_t i = 0; i < count; i++) {
        const char* name = R32(bones + i * 12) ? &G<char>(R32(bones + i * 12)) : "";
        int parent = int(R32(bones + i * 12 + 8));
        if (g.neck < 0 && Has(name, "Neck")) g.neck = int(i), g.hide[i] = true;
        else if (Has(name, "Backpack")) g.hide[i] = true;
        else if (parent >= 0 && parent < int(i) && g.hide[parent]) g.hide[i] = true;
    }
    g.bones = count;
}

}  // namespace

X86_FUNC(sub_00245A20) {
    uint32_t ref = ctx.eax;
    uint32_t owner = R32(ref + 0xD8);
    if (!owner) return __imp__sub_00245A20(ctx, base);
    X86HostFp host_fp;
    uint32_t now = R32(kLevelTime);
    if (now < g.last_time) g.handle = 0, g.next_try = 0;  // the map restarted
    g.last_time = now;
    uint32_t body = Body(), player = Player(), scene = R32(kScene);
    if (!body && player && now >= g.next_try) {  // any of the level's 1st Infantry men will do, in sight or not
        uint32_t soldier = 0;
        for (uint32_t i = 0; i < kEntityCount && !soldier; i++) {
            uint32_t ent = R32(kEntities + i * 8);
            if (ent && G<uint8_t>(ent) == 11 && R32(ent + 0x258) && Has(ClassName(ent), "1stInf")) soldier = ent;
        }
        if (!soldier || !Spawn(ctx, soldier, player)) g.next_try = now + 500;
        return __imp__sub_00245A20(ctx, base);
    }
    if (!body || owner != body || !player || !scene) return __imp__sub_00245A20(ctx, base);

    // where the player is and how: the eye's height says standing, crouched or lying
    float px = G<float>(player + 0x140), py = G<float>(player + 0x144), pz = G<float>(player + 0x148);
    float cx = G<float>(scene + 0x250) - px, cy = G<float>(scene + 0x254) - py, eye = G<float>(scene + 0x258) - pz;
    float fx = G<float>(scene + 0x260), fy = G<float>(scene + 0x264);
    float len = std::sqrt(fx * fx + fy * fy);
    bool shown = cx * cx + cy * cy < 40.0f * 40.0f && eye > 25.0f && eye < 80.0f && len > 0.01f &&
                 int32_t(R32(player + 0x350)) > 0;
    auto t = std::chrono::steady_clock::now();
    float frame = std::clamp(std::chrono::duration<float>(t - g.drawn).count(), 0.0f, 0.1f);
    g.drawn = t;
    float dt = std::chrono::duration<float>(t - g.at).count();
    if (dt > 0.05f) {
        g.speed = dt < 1.0f ? std::sqrt((px - g.x) * (px - g.x) + (py - g.y) * (py - g.y)) / dt : 0.0f;
        g.x = px, g.y = py, g.at = t;
        bool moving = g.speed > kMoving;
        Play(ctx, eye > 50.0f ? (moving ? kRun : kStand) : (moving ? kCrouchRun : kCrouch), 0.2f);
    }
    for (uint32_t k = 0; k < 12; k += 4) {  // the entity goes where the player goes
        W32(body + 0x140 + k, R32(player + 0x140 + k));
        W32(body + 0x20 + k, R32(player + 0x140 + k));
    }
    if (!shown) {  // out of every view
        G<float>(ref + 0xC4) = pz - 100000.0f;
        return __imp__sub_00245A20(ctx, base);
    }
    fx /= len, fy /= len;
    const float axis[9] = {fx, fy, 0, -fy, fx, 0, 0, 0, 1};
    std::memcpy(&G<float>(ref + 0x94), axis, sizeof(axis));
    // It hangs from the camera, not from the entity: the entity moves at the server's rate, the camera every
    // frame, and a body tied to the entity jumps about under a moving camera. It stands far enough behind the eye
    // that the bottom edge of the view stops short of the collar, which is wider the wider the field of view.
    float half = std::atan(1.0f / std::max(G<float>(scene + 0x24), 0.05f));  // half the view's height, radians
    float want = std::clamp(kReach * std::tan(std::max(half - 0.09f, 0.0f)), kBehind, kFarBehind);
    want += kLean * std::min(g.speed / 150.0f, 1.0f);  // the run leans the chest forward
    float height = eye > 50.0f ? kStanding : kCrouching;
    if (g.behind == 0) g.behind = want, g.height = height;
    float ease = std::min(frame * 8.0f, 1.0f);
    g.behind += (want - g.behind) * ease, g.height += (height - g.height) * ease;
    G<float>(ref + 0xBC) = px + cx - fx * g.behind;
    G<float>(ref + 0xC0) = py + cy - fy * g.behind;
    G<float>(ref + 0xC4) = pz + eye - g.height;
    uint32_t dobj = R32(ref + 0xD4);
    uint32_t model = dobj ? R32(dobj + 0x38) : 0, matrices = dobj ? R32(dobj + 0x20) : 0;
    if (model && model != g.model) FindBones(model);
    if (model && matrices && g.neck >= 0) {
        const float* at = &G<float>(matrices + 0x30 + uint32_t(g.neck) * 0x40);
        float point[3] = {at[12], at[13], at[14]};
        for (uint32_t i = 0; i < g.bones; i++)
            if (g.hide[i]) {
                float* m = &G<float>(matrices + 0x30 + i * 0x40);
                std::memset(m, 0, 12 * sizeof(float));
                m[12] = point[0], m[13] = point[1], m[14] = point[2] - kSunk;
            }
    }
    __imp__sub_00245A20(ctx, base);
}
