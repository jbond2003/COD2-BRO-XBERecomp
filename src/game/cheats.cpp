// Cheats. The engine keeps its developer commands (it is a Quake III descendant): god (0x251640), noclip (0x2514F0),
// notarget (0x2515A0), ufo (0x251450) and give (check 0x250B30) answer "GAME_CHEATSNOTENABLED" unless the game's
// copy of sv_cheats, [0x954F34], is set; cheat variables (timescale, g_speed, g_gravity, cg_thirdPerson) are held
// to their defaults unless sv_cheats itself is (the variable at [0x940358], its integer at +0x20, value at +0x1C;
// it is read only, so no console line sets it). With the password typed in the settings panel both are kept at 1.
// The cheats themselves write the player's own fields: the console's "give" hands out every weapon.
// (The game notes what its own commands turned on: god [0x95F678], noclip [0x95F679], notarget [0x95F67A], bytes.)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>

#include "common/cheats.h"
#include "common/mouse_keyboard.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace game::cheats {

namespace {

constexpr uint32_t kGameCheats = 0x954F34, kCheatsVar = 0x940358, kFrontEnd = 0x95F8F8;
constexpr uint32_t kEntities = 0x97A968, kEntityCount = 0x540;  // {entity, generation} each
constexpr char kCode[] = "brooklyn";

std::atomic<bool> g_unlocked{false};
std::atomic<bool> g_on[kToggles];
std::atomic<bool> g_ammo{false};
std::atomic<bool> g_menus{false};
std::atomic<int64_t> g_message_at{0};  // when the message was set, milliseconds; 0 never
std::atomic<const char*> g_message{nullptr};
std::atomic<bool> g_heal{false};

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Say(const char* text) {
    g_message.store(text, std::memory_order_relaxed);
    g_message_at.store(NowMs(), std::memory_order_relaxed);
}

// The player: the one entity with a client state ([ent+0x23C]), as the command dispatcher (0x2507D0) tells a
// player from the rest.
uint32_t Player() {
    for (uint32_t i = 0; i < kEntityCount; i++) {
        uint32_t ent = kernel::R32(kEntities + i * 8);
        if (ent && kernel::R32(ent + 0x23C)) return ent;
    }
    return 0;
}

}  // namespace

bool Unlocked() { return g_unlocked.load(std::memory_order_relaxed); }

bool TryUnlock(const char* code) {
    if (!code || std::strlen(code) != sizeof(kCode) - 1) return false;
    for (size_t i = 0; i < sizeof(kCode) - 1; i++)
        if (std::tolower(static_cast<unsigned char>(code[i])) != kCode[i]) return false;
    if (!g_unlocked.exchange(true)) LOGR("cheats: unlocked");
    return true;
}

bool On(Toggle t) { return g_on[t].load(std::memory_order_relaxed); }
void Set(Toggle t, bool on) {
    if (Unlocked()) g_on[t].store(on, std::memory_order_relaxed);
}

bool InMenus() { return g_menus.load(std::memory_order_relaxed); }

const char* Message(double& age) {
    int64_t at = g_message_at.load(std::memory_order_relaxed);
    age = at ? double(NowMs() - at) / 1000.0 : -1.0;
    return g_message.load(std::memory_order_relaxed);
}

void FullHealth() {
    if (Unlocked()) g_heal.store(true, std::memory_order_relaxed);
}

bool UnlimitedAmmo() { return g_ammo.load(std::memory_order_relaxed); }
void SetUnlimitedAmmo(bool on) { g_ammo.store(on, std::memory_order_relaxed); }

// A profile named "thereisacow1337" (any case) has every mission, and gets a message on the screen the once, when it is
// made and unlocked. The profile in use is the block at 0x8DCE30, its name first, the chapters done at
// +0x14 ([0x8DCE44], -1 none, 14 all) and the everything-unlocked byte at +0x24 ([0x8DCE54]); the game's own unlock
// at 0x30CC44 sets the same two. They are kept set while that profile is the one in use, so it is saved with it
// too. The panel's thread draws the message (menu.cpp).
void NamedUnlock() {
    constexpr uint32_t kProfile = 0x8DCE30, kChapters = 0x8DCE44, kEverything = 0x8DCE54;
    constexpr char kName[] = "thereisacow1337";
    for (uint32_t i = 0; i < sizeof(kName); i++)
        if (std::tolower(kernel::G<uint8_t>(kProfile + i)) != uint8_t(kName[i])) return;
    if (int32_t(kernel::R32(kChapters)) == 14 && kernel::G<uint8_t>(kEverything) == 1) return;
    kernel::W32(kChapters, 14);
    kernel::G<uint8_t>(kEverything) = 1;
    // once: the profile is saved unlocked. (The game puts the two back a few times while the profile is being
    // made, so the message is also only ever shown once in a run.)
    static bool said = false;
    if (!said) Say("Moo.");
    said = true;
    LOGR("profile thereisacow1337: every mission unlocked");
}

// Unlimited ammo: the player's counts never fall. ("give ammo" is not it: on this build it hands out weapons too.)
// The player's state is at [ent+0x23C]: the weapon in hand at +0x80, the rounds in reserve
// at +0x110 and in the clips at +0x280, one int per pool. A weapon's pools are in its definition ([0x95F4D8][n],
// n from 1 to [0x95F4D4]): +0x598 the reserve's, +0x5A0 the clip's (as the engine's "take" reads them, 0x253500).
// Each pool is held at the most it has had since the cheat went on in this mission; picking ammo up raises it.
void KeepAmmo(uint32_t ent) {
    constexpr uint32_t kWeaponCount = 0x95F4D4, kWeapons = 0x95F4D8;
    constexpr uint32_t kPools = 64;
    static int32_t most[2][kPools];
    static uint32_t state = 0;
    static int32_t time = 0;
    uint32_t ps = ent ? kernel::R32(ent + 0x23C) : 0;
    uint32_t defs = kernel::R32(kWeapons);
    int32_t count = int32_t(kernel::R32(kWeaponCount));
    uint32_t held = ps ? kernel::R32(ps + 0x80) : 0;  // a state with no sane weapon in hand is left alone
    if (!ps || !defs || count < 1 || held > uint32_t(count)) {
        state = 0;
        return;
    }
    int32_t now = int32_t(kernel::R32(ps + 0x20));  // the state's own clock: back at a restart or a new mission
    if (ps != state || now < time) std::memset(most, 0, sizeof(most));
    state = ps, time = now;
    for (int32_t n = 1; n <= count && n < 512; n++) {
        uint32_t def = kernel::R32(defs + uint32_t(n) * 4);
        if (!def) continue;
        const uint32_t pool[2] = {kernel::R32(def + 0x598), kernel::R32(def + 0x5A0)};
        const uint32_t at[2] = {ps + 0x110, ps + 0x280};
        for (int k = 0; k < 2; k++) {
            if (pool[k] >= kPools) continue;
            uint32_t slot = at[k] + pool[k] * 4;
            int32_t have = int32_t(kernel::R32(slot));
            int32_t& top = most[k][pool[k]];
            if (have > top) top = have;
            else if (have < top) kernel::W32(slot, uint32_t(top));
        }
    }
}

// God mode and enemies ignoring the player are bits 1 and 2 of the player's flags ([ent+0x2A4], what the engine's
// god 0x251640 and notarget 0x2515A0 flip). The bits are kept as the tick boxes say each frame, so a restart or a
// new mission does not lose them.
void KeepFlags(uint32_t ent) {
    if (!ent) return;
    uint32_t flags = kernel::R32(ent + 0x2A4);
    uint32_t want = (flags & ~3u) | (On(kGod) ? 1u : 0u) | (On(kNoTarget) ? 2u : 0u);
    if (want != flags) kernel::W32(ent + 0x2A4, want);
}

void Tick(X86Context&) {
    g_menus.store(kernel::R32(kFrontEnd) != 0, std::memory_order_relaxed);
    NamedUnlock();
    if (!Unlocked()) return;
    kernel::W32(kGameCheats, 1);
    if (uint32_t var = kernel::R32(kCheatsVar)) {
        kernel::W32(var + 0x20, 1);
        kernel::G<float>(var + 0x1C) = 1.0f;
    }
    bool level = !kernel::R32(kFrontEnd);
    uint32_t player = level ? Player() : 0;
    static uint32_t seen = 0;
    if (player && player != seen) LOGR("cheats: the player is entity %08X, state %08X", player, kernel::R32(player + 0x23C));
    seen = player;
    KeepAmmo(UnlimitedAmmo() ? player : 0);
    KeepFlags(player);
    // Full health, as "give health" has it (0x26D546): the most a player has ([0x950CEC]) in the entity and the state
    if (g_heal.exchange(false) && player && int32_t(kernel::R32(player + 0x350)) > 0) {
        uint32_t full = kernel::R32(0x950CEC);
        if (int32_t(full) > 0) {
            kernel::W32(player + 0x350, full);
            kernel::W32(kernel::R32(player + 0x23C) + 0x108, full);
            LOGR("cheats: full health, %u", full);
        }
    }
}

}  // namespace game::cheats

// ClientThink, the player's state (ebx) about to take the world's gravity (0x271D8D "mov [ebx+0x48], eax", eax
// g_gravity) and run speed (0x271D96 "mov [ebx+0x50], ecx", ecx g_speed). The cheats change the two here, for the
// player alone: g_gravity itself would also slow every grenade, body and vehicle.
void CheatGravity(X86Context& ctx, uint8_t*) {
    using namespace game::cheats;
    if (Unlocked() && On(kLowGravity)) ctx.eax = 200;
}

void CheatRunSpeed(X86Context& ctx, uint8_t*) {
    using namespace game::cheats;
    if (Unlocked() && On(kFastRun)) ctx.ecx *= 2;
}
