// Texts of the game that speak of the console, said as on a PC. They are entries of the global string table
// (sp/global.cod), found by the game through its hash table lookup (0x2984F0: eax table, the key's hash on the stack;
// the entry, whose +8 is the text). A text found there is rewritten in place in the table, the new text being no
// longer than the old, before the game uses it: nothing is searched for.

#include <cstring>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

struct Text {
    const char* xbox;
    const char* pc;
};

constexpr Text kTexts[] = {
    {"Checking hard disk. Please do not turn off your Xbox console.", "Checking storage. Please do not turn off your PC."},
    {"Loading profile. Please do not turn off your Xbox console.", "Loading profile. Please do not turn off your PC."},
    {"Saving Profile. Please don't turn off your Xbox console.", "Saving profile. Please do not turn off your PC."},
    {"Overwriting game. Please do not turn off your Xbox console.", "Overwriting game. Please do not turn off your PC."},
    {"Deleting game. Please do not turn off your Xbox console.", "Deleting game. Please do not turn off your PC."},
    {"Formatting hard drive. Please do not turn off your Xbox console.",
     "Formatting storage. Please do not turn off your PC."},
    {"No saved games present on hard disk.", "No saved games present."},
    {"All progress in this level will be lost. Are you sure you wish to exit to the Xbox Dashboard?",
     "All progress in this level will be lost. Are you sure you wish to exit to the desktop?"},
};

bool IsGuest(uint32_t va) { return va >= mem::kContiguousBase && va - mem::kContiguousBase < mem::kPhysicalSize - 256; }

// the entry's text, if one of ours, put as on a PC
void Check(uint32_t entry) {
    if (!IsGuest(entry)) return;
    uint32_t text = kernel::R32(entry + 8);
    if (!IsGuest(text)) return;
    char* p = static_cast<char*>(kernel::HostPtr(text));
    for (const Text& t : kTexts) {
        if (p[0] != t.xbox[0] || std::strcmp(p, t.xbox) != 0) continue;
        size_t old_len = std::strlen(t.xbox);
        std::memset(p, 0, old_len);
        std::memcpy(p, t.pc, std::strlen(t.pc));
        LOGR("PC text: \"%s\"", t.pc);
        return;
    }
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002984F0);

// the hash table lookup (eax table, the hash on the stack; eax the entry or 0)
X86_FUNC(sub_002984F0) {
    __imp__sub_002984F0(ctx, base);
    if (ctx.eax) Check(ctx.eax);
}
