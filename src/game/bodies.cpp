// Bodies that stay, and cutscenes without their black bars (PC Options: Graphics, Display).
//
// Bodies. A soldier who dies becomes a corpse entity (Actor_BecomeCorpse 0x2133A0, entity type 13) and takes a slot
// of g_actorCorpses (0x9881C8, 16 of 0x1C bytes: the entity's handle, then its lean on the ground), of which
// ai_corpseCount ([0x985418], 10) are in use. With no slot free, Actor_FreeFarthestCorpse (0x204C70) frees the body
// farthest from the player and hands its slot out: the eleventh body makes one disappear. With the setting on the
// farthest body only gives up its slot and stays in the world. The game finds a corpse's slot by its handle and
// takes slot 0 when there is none (0x200300, 0x211DC4, 0x2EDC49), so a body without a slot is nothing new to it: a
// loaded checkpoint brings one back as it would any entity. The entities are few (0x540 handles), so at most kKept
// bodies stay beyond the game's own, and past that the farthest of them is freed as the game would have.
//
// Bars. The level scripts' cutscenes call BroScr_LetterboxFade (0x141890): letterbox_enabled 1, no gun, no HUD, and
// two "black" HUD elements 640 wide and 70 high, the top one placed at y 0 (0x141A03) and the bottom one at 410
// (0x141BBA). With the setting off they are placed off the picture: the cutscene is the same but for the bars.

#include <algorithm>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

X86_EXTERN_FUNC(__imp__sub_00204C70);

namespace {

constexpr uint32_t kCorpses = 0x9881C8, kCorpseBytes = 0x1C, kCorpseSlots = 16, kCorpsesInUse = 0x985418;
constexpr uint32_t kEntities = 0x97A968, kEntityCount = 0x540;  // {entity, generation} each
constexpr uint32_t kFreeEntity = 0x2BE420;                      // G_FreeEntity (entity, 0), cdecl
constexpr uint8_t kCorpseType = 13;
constexpr size_t kKept = 64;

using kernel::R32;
using kernel::W32;

// the entity of a handle (index in the low 12 bits, generation above), 0 when it is gone
uint32_t Entity(uint32_t handle) {
    uint32_t index = handle & 0xFFF;
    if (!handle || index >= kEntityCount || R32(kEntities + index * 8 + 4) != handle >> 12) return 0;
    uint32_t ent = R32(kEntities + index * 8);
    return ent && R32(ent + 0x224) == handle ? ent : 0;
}

struct Place {
    float x = 0, y = 0, z = 0;
};
Place Origin(uint32_t ent) { return {kernel::G<float>(ent + 0x140), kernel::G<float>(ent + 0x144), kernel::G<float>(ent + 0x148)}; }
float Apart(const Place& a, const Place& b) {
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return x * x + y * y + z * z;
}

// the player: the first entity with a player's state (as the game's command dispatcher 0x2507D0 tests)
Place Player() {
    for (uint32_t i = 0; i < kEntityCount; i++) {
        uint32_t ent = R32(kEntities + i * 8);
        if (ent && R32(ent + 0x23C)) return Origin(ent);
    }
    return {};
}

bool HasSlot(uint32_t handle) {
    for (uint32_t i = 0; i < kCorpseSlots; i++)
        if (R32(kCorpses + i * kCorpseBytes) == handle) return true;
    return false;
}

// no more than kKept bodies without a slot: the farthest go. (The soldier becoming a body now has no slot yet.)
void Prune(X86Context& ctx, const Place& player, uint32_t dying) {
    struct Body {
        uint32_t ent;
        float apart;
    };
    std::vector<Body> bodies;
    for (uint32_t i = 0; i < kEntityCount; i++) {
        uint32_t ent = R32(kEntities + i * 8);
        if (!ent || ent == dying) continue;
        uint32_t handle = R32(ent + 0x224);
        if (Entity(handle) != ent || (handle & 0xFFF) != i || kernel::G<uint8_t>(ent) != kCorpseType) continue;
        if (!HasSlot(handle)) bodies.push_back({ent, Apart(Origin(ent), player)});
    }
    while (bodies.size() > kKept) {
        size_t far = 0;
        for (size_t i = 1; i < bodies.size(); i++)
            if (bodies[i].apart > bodies[far].apart) far = i;
        // freeing a corpse clears its slot, and slot 0 for a body that has none (0x2EDC49): slot 0 is kept
        uint32_t first = R32(kCorpses);
        kernel::CallGuest(ctx, kFreeEntity, {bodies[far].ent, 0});
        W32(kCorpses, first);
        bodies.erase(bodies.begin() + far);
    }
}

}  // namespace

// Actor_FreeFarthestCorpse: () -> eax, a free slot of g_actorCorpses. Its one caller, Actor_BecomeCorpse, has the
// soldier in esi (0x213515).
X86_FUNC(sub_00204C70) {
    if (!game::Opt().keep_bodies) return __imp__sub_00204C70(ctx, base);
    uint32_t in_use = std::min(std::max(R32(kCorpsesInUse), 1u), kCorpseSlots);
    Place player = Player();
    uint32_t far = 0;
    float most = -1.0f;
    for (uint32_t i = 0; i < in_use; i++) {
        uint32_t ent = Entity(R32(kCorpses + i * kCorpseBytes));
        if (!ent) return kernel::Return(ctx, i, 0);  // free
        float apart = Apart(Origin(ent), player);
        if (apart > most) most = apart, far = i;
    }
    static bool said = false;
    if (!said) LOGR("bodies stay: the first body to keep after the game's own %u", in_use);
    said = true;
    W32(kCorpses + far * kCorpseBytes, 0);  // the body stays, without a slot
    Prune(ctx, player, ctx.esi);
    kernel::Return(ctx, far, 0);
}

// BroScr_LetterboxFade, at its calls that place a bar: (element, y) on the stack. Off the picture, the bars are
// not seen (a bar of no height is drawn 16 high).
void CutsceneBarTop(X86Context& ctx, uint8_t*) {
    if (!game::Opt().cutscene_bars) W32(ctx.esp + 4, uint32_t(-400));
}
void CutsceneBarBottom(X86Context& ctx, uint8_t*) {
    if (!game::Opt().cutscene_bars) W32(ctx.esp + 4, 880);
}
