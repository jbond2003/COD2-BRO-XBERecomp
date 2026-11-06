// Sprint in single player. The game registers +sprint and -sprint (0x2ABEC0, 0x2ABEB0) as empty functions, the
// multiplayer image's bodies gone, but keeps their button: in_buttons[3] (0x934068), which CL_CmdButtons (0x2ABC30)
// still turns into usercmd button 0x08, and the player movement still sprints on it (PM_UpdateSprint 0x2C2B40:
// pm_flags 0x10000 sprinting, ps+0x45C the meter, ps+0x460 the last sprint time). The two commands press and release
// that button again (IN_KeyDown 0x2ABFE0, IN_KeyUp 0x2ABF50, the button in esi).
//
// The meter is multiplayer's: full at 1.0, drained at 2/3 a second while sprinting, the sprint ended below 0.5
// (about a second of running), refilled at 1/3 a second after a second's rest. The game keeps a switch that skips the
// drain, the static at 0x92E79C (read by PM_UpdateSprint alone, never written by this image): set, sprint lasts as
// long as the button is held, as in the later games' campaigns.
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {
constexpr uint32_t kSprintButton = 0x934068;
constexpr uint32_t kKeyDown = 0x2ABFE0, kKeyUp = 0x2ABF50;
constexpr uint32_t kNoSprintDrain = 0x92E79C;

void SprintKey(X86Context& ctx, uint32_t fn) {
    kernel::W32(kNoSprintDrain, 1);
    uint32_t esi = ctx.esi;
    ctx.esi = kSprintButton;
    kernel::CallGuest(ctx, fn, {});
    ctx.esi = esi;
    ctx.esp += 4;  // the command's own ret
}
}  // namespace

X86_FUNC(sub_002ABEC0) { SprintKey(ctx, kKeyDown); }  // +sprint
X86_FUNC(sub_002ABEB0) { SprintKey(ctx, kKeyUp); }    // -sprint
