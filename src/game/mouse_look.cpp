// Mouse look, and the keys following the controller layout picked in the game's options.
//
// The Xbox build feeds its right stick through Quake 3's mouse path (IN_JoyMove posts mouse events, CL_MouseMove
// 0x2B39E0 applies stick acceleration and a turn limit), so a real mouse cannot go through there. Before CL_CreateCmd
// calls CL_MouseMove, the mouse motion is added to cl.viewangles the way PC Call of Duty does, under the conditions
// CL_MouseMove checks. Addresses after CoD2-BRO-Native-Loader (GPL-2.0-or-later, THIRD_PARTY_NOTICES.md).

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

#include "common/cheats.h"
#include "common/menu_state.h"
#include "common/mouse_keyboard.h"
#include "core/input.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

namespace {

constexpr uint32_t kViewFlags = 0x9601AC;       // player state: 0x4000 view locked
constexpr uint32_t kViewAllowFlags = 0x960240;  // ... unless one of 0x106000
constexpr uint32_t kVehicle = 0x933FB8;         // non-zero in a vehicle: its own controls
constexpr uint32_t kSensitivity = 0x961774;     // cl.cgameSensitivity: lower down the sights, 0 before cgame's frame
constexpr uint32_t kPitch = 0x961798, kYaw = 0x96179C;  // cl.viewangles
constexpr uint32_t kFreelook = 0x9341B0;        // non-zero: pitch follows the mouse
constexpr uint32_t kClFreelook = 0x93F808, kMPitch = 0x929C0C;  // cvar_t pointers
constexpr uint32_t kKeyBindings = 0x93425C;     // keys[code].binding, 8 bytes a key: the command string or null
constexpr float kDegreesPerCount = 0.11f;

float& F(uint32_t a) { return kernel::G<float>(a); }
// a cvar_t pointer's value (+0x1C) and integer (+0x20)
float CvarValue(uint32_t cvar) {
    uint32_t c = kernel::R32(cvar);
    return c ? kernel::G<float>(c + 0x1C) : 0.0f;
}
int32_t CvarInt(uint32_t cvar) {
    uint32_t c = kernel::R32(cvar);
    return c ? int32_t(kernel::R32(c + 0x20)) : 0;
}

void MouseLook() {
    if (game::MenuHasPointer()) return;
    float dx, dy;
    if (!input::TakeMouse(dx, dy)) return;
    if ((kernel::R32(kViewFlags) & 0x4000) && !(kernel::R32(kViewAllowFlags) & 0x106000)) return;
    if (kernel::R32(kVehicle)) return;
    float scale = kDegreesPerCount * F(kSensitivity);
    if (game::cheats::Unlocked() && game::cheats::On(game::cheats::kBackwards)) dx = -dx, dy = -dy;  // sdrawkcaB
    F(kYaw) -= dx * scale;
    if (kernel::R32(kFreelook) || CvarInt(kClFreelook)) {
        float invert = CvarValue(kMPitch) < 0 ? -1.0f : 1.0f;  // the game's own Invert Aim
        F(kPitch) += dy * scale * invert;
    }
}

// The keyboard gives the pad controls of the default layout (config/default.cfg); the layouts the options offer
// (style_a..l.cfg) bind the controls to other commands. Each key's control moves to the one bound to its command
// now, else to one bound to a similar command, else its command is run itself (reload and use stay apart where the
// layout joins them on one button). The lean keys always lean, as on the PC: never the "lean, or switch weapons
// when not aiming" the dpad has.
struct PadControl {
    uint16_t digital;     // Pad::buttons bit, 0 for an analog button
    uint8_t analog;       // Pad::analog index when digital is 0
    uint8_t key_code;     // the game's key code for it
    const char* command;  // its command in the default layout
};
constexpr PadControl kPadControls[] = {
    {0, 0, 0xD4, "+activate"},         {0, 1, 0xD5, "lowerstance"},     {0, 2, 0xD3, "+reload"},
    {0, 3, 0xD6, "+moveup"},           {0, 4, 0xD9, "+melee"},          {0, 5, 0xDA, "+grenadeattack"},
    {0, 6, 0xD8, "+speed"},            {0, 7, 0xD7, "+attack"},         {0x80, 0, 0xDB, "toggle cl_run"},
    {0x40, 0, 0xDC, "+sprint"},        {0x01, 0, 0x9A, "weapnext"},     {0x02, 0, 0x9B, ""},
    {0x04, 0, 0x9C, "+leanleftswitchnext"}, {0x08, 0, 0x9D, "+leanrightswitchnext"},
};

const char* Similar(const char* command) {
    struct Pair {
        const char* command;
        const char* similar;
    };
    static constexpr Pair kSimilar[] = {{"weapnext", "weapprev"}};
    for (const Pair& p : kSimilar)
        if (std::strcmp(p.command, command) == 0) return p.similar;
    return nullptr;
}

// a key's binding, at most 63 characters
void Binding(uint8_t code, char* out) {
    uint32_t s = kernel::R32(kKeyBindings + uint32_t(code) * 8);
    int i = 0;
    for (; s && i < 63; i++) {
        char c = char(kernel::G<uint8_t>(s + uint32_t(i)));
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

bool SameCommand(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20)) return false;
    return *a == *b;
}

uint8_t Value(const input::Pad& p, const PadControl& c) { return c.digital ? ((p.buttons & c.digital) ? 0xFF : 0) : p.analog[c.analog]; }
void Press(input::Pad& p, const PadControl& c, uint8_t v) {
    if (c.digital) p.buttons |= c.digital;
    else p.analog[c.analog] = std::max(p.analog[c.analog], v);
}

}  // namespace

namespace game {

// the keyboard's pad of the default layout onto the layout the game has now
void RemapToLayout(const input::Pad& from, input::Pad& to) {
    char bound[std::size(kPadControls)][64];
    bool any = false;
    for (size_t i = 0; i < std::size(kPadControls); i++) {
        Binding(kPadControls[i].key_code, bound[i]);
        any = any || bound[i][0];
    }
    if (!any) {  // no bindings yet: the default layout
        for (const PadControl& c : kPadControls)
            if (uint8_t v = Value(from, c)) Press(to, c, v);
        return;
    }
    // a control the layout has no button for (leaning in Modern) runs its command itself
    static bool commanded[std::size(kPadControls)] = {};
    for (size_t k = 0; k < std::size(kPadControls); k++) {
        const PadControl& c = kPadControls[k];
        uint8_t v = Value(from, c);
        const PadControl* target = nullptr;
        const char* own = c.key_code == 0x9C ? "+leanleft" : c.key_code == 0x9D ? "+leanright" : nullptr;
        for (const char* command : {c.command, Similar(c.command)}) {
            if (own || !command || !*command) continue;
            for (size_t i = 0; i < std::size(kPadControls); i++)
                if (SameCommand(bound[i], command)) target = &kPadControls[i];
            if (target) break;
        }
        const char* command = own ? own : c.command;
        bool direct = v && !target && *command && c.key_code != 0xD5;  // not lower stance: the stance keys send theirs
        if (direct != commanded[k]) {
            if (command[0] == '+') input::QueueCommand((direct ? "" : "-") + std::string(direct ? command : command + 1));
            else if (direct) input::QueueCommand(command);
            commanded[k] = direct;
        }
        if (v && target) Press(to, *target, v);
    }
}

// where RemapToLayout sends default control i (kPadControls' order: A B X Y black white LT RT, right and left stick
// clicks, dpad up down left right): the index of the control it presses now, -1 for none
int PadTarget(size_t i) {
    if (i >= std::size(kPadControls)) return -1;
    char bound[std::size(kPadControls)][64];
    bool any = false;
    for (size_t k = 0; k < std::size(kPadControls); k++) {
        Binding(kPadControls[k].key_code, bound[k]);
        any = any || bound[k][0];
    }
    if (!any) return int(i);
    const PadControl& c = kPadControls[i];
    if (c.key_code == 0x9C || c.key_code == 0x9D) return -1;  // the lean keys run their command
    for (const char* command : {c.command, Similar(c.command)}) {
        if (!command || !*command) continue;
        int target = -1;
        for (size_t k = 0; k < std::size(kPadControls); k++)
            if (SameCommand(bound[k], command)) target = int(k);
        if (target >= 0) return target;
    }
    return -1;
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_002B39E0);

// CL_MouseMove, from CL_CreateCmd
X86_FUNC(sub_002B39E0) {
    {
        X86HostFp host_fp;
        MouseLook();
        game::WatchVehicles();
    }
    __imp__sub_002B39E0(ctx, base);
}
