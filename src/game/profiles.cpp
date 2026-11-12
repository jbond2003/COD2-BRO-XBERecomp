// Deleting a profile. The game has it, on the profile screen (vtable 0x828ED0): X asks "Are you sure you wish to
// delete this save file?" (0x334A20, the screen's vtable +0x68: MEM_DELETE_CONFIRM_XBOX, Yes 0x334960 clears the
// slot and writes the Profiles files, "Delete Successful" after), and the foot of the screen says so
// (FEMENU_LOAD_HELPBAR_XBOX, "Press A to select X to delete and B to go back"). But only in the screen's mode 0, which
// is Campaign > Save Profile, where nobody looks for it. The Profile Manager (mode 1: the main menu's B, and at boot)
// and Campaign > Switch Profile (mode 2) offer it here as well; the confirmation, the write and the dialogs after
// it are the game's own and do not look at the mode. Mode 3 (a profile just made, picking its slot) stays as it was.

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

bool OffersDelete(uint32_t screen) {
    uint32_t mode = kernel::R32(screen + 0x4C);
    return mode == 1 || mode == 2;
}

}  // namespace

// 0x33A64D, the profile screen's update (esi the screen) about to choose its foot text by the mode: true takes mode
// 0's way, the delete line on a slot that holds a profile
bool ProfileDeleteHelp(X86Context& ctx, uint8_t*) { return OffersDelete(ctx.esi); }

// 0x334A31, X on the profile screen (ecx the screen) about to refuse outside mode 0: true goes on to the slot's
// check and the confirmation
bool ProfileDeleteAllowed(X86Context& ctx, uint8_t*) {
    if (!OffersDelete(ctx.ecx)) return false;
    LOGR("profile screen: X (delete) on slot %d, mode %u", int(int16_t(kernel::G<uint16_t>(ctx.ecx + 0x22))),
         kernel::R32(ctx.ecx + 0x4C));
    return true;
}
