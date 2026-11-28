// The Audio settings: how loud the music, the main menu's music, the sound effects and the dialogue are.
//
// The game mixes through named sound groups: a table at 0x8BD8F0, [0x8BEB70] of them, 0x128 bytes each, the name at
// +0x108, flags at +0 and the group's level at +8. They are NSL_MASTER, NSL_LISTENER, VOICE, GUI, MUSIC and SFX.
// The game's own Volume setting is the master's (S_SetMasterVolume 0xC6490: the level, flag 1, and the counter at
// 0x8BD650 made negative, which has the mixer take the groups' levels again); it leaves the other groups at 1 from
// the start. So the settings here are those groups' levels, set the same way: MUSIC from the music setting in a
// mission and the main menu music's in the menus (the front end, [0x95F8F8]), SFX and GUI from the effects', VOICE
// from the dialogue's. A setting is a percentage; the level is its square, which is how loudness is heard.

#include <cstring>

#include "common/options.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace game {

namespace {

constexpr uint32_t kGroups = 0x8BD8F0, kGroupCount = 0x8BEB70, kGroupSize = 0x128, kGroupName = 0x108;
constexpr uint32_t kGroupsChanged = 0x8BD650, kFrontEnd = 0x95F8F8;

void SetGroup(const char* name, uint32_t percent, bool& changed) {
    uint32_t count = kernel::R32(kGroupCount);
    for (uint32_t i = 0; i < count && i < 16; i++) {
        uint32_t group = kGroups + i * kGroupSize;
        if (std::strncmp(static_cast<const char*>(kernel::HostPtr(group + kGroupName)), name, 0x20) != 0) continue;
        float part = float(percent > 100 ? 100 : percent) / 100.0f, level = part * part;
        float& now = kernel::G<float>(group + 8);
        if (now == level) return;
        now = level;
        kernel::W32(group, kernel::R32(group) | 1);
        changed = true;
        return;
    }
}

}  // namespace

// each client frame
void ApplyVolumes() {
    const Options& o = Opt();
    bool changed = false;
    SetGroup("MUSIC", kernel::R32(kFrontEnd) ? o.menu_music_volume : o.music_volume, changed);
    SetGroup("SFX", o.effects_volume, changed);
    SetGroup("GUI", o.effects_volume, changed);
    SetGroup("VOICE", o.voice_volume, changed);
    if (!changed) return;
    // as S_SetMasterVolume ends: the mixer takes the levels again
    int32_t counter = int32_t(kernel::R32(kGroupsChanged));
    kernel::W32(kGroupsChanged, uint32_t(-(counter < 0 ? -counter : counter)));
}

}  // namespace game
