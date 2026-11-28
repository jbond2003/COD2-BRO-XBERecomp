// The game's own options in the settings menu. The pause menu's Options (applied by 0x331340) and the Controller
// screen (0x3361C0, line 6) keep them in the profile in memory (0x8DCE30): subtitles +0x38, the crosshair +0x39,
// friendly name tags +0x3A, the alternate tank control +0x3C (a dword, g_vehControlMode), the ADS toggle +0x40,
// invert aim +0x41 (m_pitch's sign), vibration +0x42, the look sensitivities +0x44 and +0x48, the controller
// configuration +0x4C and the volume +0x58 (0 to 50). Most are read where they are used; the rest are applied by
// CL_ApplyProfileSettings (0x332E20, edi the profile), which is run here again after a change.
// The game writes its profile only now and then (making it, finishing a mission), so the values are kept in the
// settings file as well and put back whenever a profile is applied (src/game/pad_layout.cpp).

#include <atomic>

#include "app/app.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"

namespace game {
namespace {

using kernel::R32;
using kernel::W32;

constexpr uint32_t kProfile = 0x8DCE30;
constexpr uint32_t kApplyProfile = 0x332E20;
constexpr uint32_t kInGame = 0x95F8FC;  // the in game screens' manager: its first screen's +0x40 is the crosshair's own flag
constexpr int kModern = 4;

struct Field {
    uint32_t offset;
    bool byte;
};
constexpr Field kFields[kGameOptions] = {
    {0x38, true}, {0x39, true}, {0x3A, true}, {0x3C, false}, {0x40, true}, {0x41, true}, {0x42, true}, {0x58, false}, {0x4C, false},
};

uint8_t R8(uint32_t a) { return *static_cast<const uint8_t*>(kernel::HostPtr(a)); }
void W8(uint32_t a, uint8_t v) { *static_cast<uint8_t*>(kernel::HostPtr(a)) = v; }

int32_t Read(uint32_t profile, int i) { return kFields[i].byte ? R8(profile + kFields[i].offset) : int32_t(R32(profile + kFields[i].offset)); }
void Write(uint32_t profile, int i, int32_t v) {
    if (kFields[i].byte) W8(profile + kFields[i].offset, uint8_t(v != 0));
    else W32(profile + kFields[i].offset, uint32_t(v));
}

bool Loaded() { return R8(kProfile) != 0; }  // the profile's name is its first field

std::atomic<bool> g_apply{false};

}  // namespace

void ApplyGameOptions() { g_apply.store(true, std::memory_order_relaxed); }

// the game's own menus just applied their lines: what the profile has now is kept
void KeepGameOptions() {
    Options& o = Opt();
    bool changed = false;
    for (int i = 0; i < kGameOptions; i++) {
        int32_t v = Read(kProfile, i);
        if (i == kGoLayout && (v < 0 || v >= kModern)) continue;  // Modern is modern_layout's (src/game/pad_layout.cpp)
        if (v != o.game_options[size_t(i)]) o.game_options[size_t(i)] = v, changed = true;
    }
    int32_t x = int32_t(R32(kProfile + 0x44)), y = int32_t(R32(kProfile + 0x48));
    if (x != o.look_sensitivity_x || y != o.look_sensitivity_y) o.look_sensitivity_x = x, o.look_sensitivity_y = y, changed = true;
    if (!changed) return;
    app::Save();
    LOGR("game options kept from the game's menu");
}

// a profile is about to be applied: what is kept goes in
void PutGameOptions(uint32_t profile) {
    if (!profile) return;
    const Options& o = Opt();
    for (int i = 0; i < kGameOptions; i++) {
        int32_t v = o.game_options[size_t(i)];
        if (i == kGoLayout && o.modern_layout) v = kModern;
        if (v >= 0) Write(profile, i, v);
    }
    if (o.look_sensitivity_x >= 0) W32(profile + 0x44, uint32_t(o.look_sensitivity_x));
    if (o.look_sensitivity_y >= 0) W32(profile + 0x48, uint32_t(o.look_sensitivity_y));
}

// each client frame: lines with nothing kept take the profile's values (for the menu to show), and a change made
// in the settings menu is applied
void UpdateGameOptions(X86Context& ctx) {
    if (!Loaded()) return;
    Options& o = Opt();
    for (int i = 0; i < kGameOptions; i++) {
        if (o.game_options[size_t(i)] >= 0) continue;
        int32_t v = Read(kProfile, i);
        if (i == kGoLayout && (v < 0 || v >= kModern)) v = 0;
        o.game_options[size_t(i)] = v;
    }
    if (o.look_sensitivity_x < 0) o.look_sensitivity_x = int32_t(R32(kProfile + 0x44));
    if (o.look_sensitivity_y < 0) o.look_sensitivity_y = int32_t(R32(kProfile + 0x48));
    if (!g_apply.exchange(false, std::memory_order_relaxed)) return;
    static bool modern = o.modern_layout;
    if (modern != o.modern_layout) {
        modern = o.modern_layout;
        gpu::texture_override::Changed();  // the Controller screen's picture (src/game/pad_picture.cpp)
    }
    // the crosshair's own flag on the HUD, as the pause menu's Options sets it
    if (uint32_t manager = R32(kInGame)) {
        uint32_t screens = R32(manager + 4);
        uint32_t hud = screens ? R32(screens) : 0;
        if (hud) W8(hud + 0x40, uint8_t(o.GameOption(kGoCrosshair) != 0));
    }
    // (the override of 0x332E20 puts the kept values in the profile first)
    X86Context saved = ctx;
    ctx.edi = kProfile;
    kernel::CallGuest(ctx, kApplyProfile, {});
    ctx = saved;
    LOGR("game options applied from the settings menu");
}

}  // namespace game
