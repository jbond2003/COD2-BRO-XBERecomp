// The Modern controller configuration, a fifth choice after the game's A to D in Options > Controller, and the
// default. The Controller screen (vtable 0x829C48) keeps its choice at +0xC0 (0-3, A-D, stepped at 0x3360E8 and kept
// to 3 at 0x336118) and copies it to [0x8DCE7C] (cg_controllerconfig); 0x335D60 (eax screen) shows its letter and
// applies it through 0x331030 (esi choice), which execs style_e/f/g.cfg for 1-3 and style_h.cfg otherwise
// (Cbuf_AddText 0x2AABB0, text in eax). Opening the screen (0x338FD0) turns a 4 into 0.
//
// Here the choice goes to 4 = Modern: its letter reads "Modern", and whenever the game execs a style while Modern is
// picked (Options().modern_layout, kept in the settings file) the binds below follow it. Modern: RT fire, LT aim
// down the sights, A jump, X use and reload, Y and the dpad's right next weapon, dpad left the one before, RB a
// grenade, R3 melee, LB nothing, L3 sprint, dpad up and Back objectives, Start the pause menu. B crouches and
// stands, held it goes prone (ModernPad: B is bound to nothing and sends the commands).
//
// Also the console commands the keyboard sends (crouch, prone, controls the layout has no button for) run here, each
// client frame.

#include <chrono>
#include <cstring>
#include <string>

#include "app/app.h"
#include "common/cheats.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/texture_override.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;
using kernel::W32;


namespace {

constexpr uint32_t kChoice = 0x8DCE7C;  // cg_controllerconfig's copy
constexpr uint32_t kCbufAddText = 0x2AABB0;
constexpr int kModern = 4;

constexpr const char* kModernBinds =
    "bind R1 \"+attack\"\n"
    "bind L1 \"+speed\"\n"
    "bind JOY6 \"+moveup\"\n"
    "bind SQUARE \"+activatereload\"\n"
    "bind TRIANGLE \"weapnext\"\n"
    "bind CIRCLE \"\"\n"
    "bind R2 \"+grenadeattack\"\n"
    "bind L2 \"\"\n"
    "bind R3 \"+melee\"\n"
    "bind L3 \"+sprint\"\n"
    "bind UPARROW \"+scores\"\n"
    "bind DOWNARROW \"\"\n"
    "bind LEFTARROW \"weapprev\"\n"
    "bind RIGHTARROW \"weapnext\"\n"
    "bind TAB \"+scores\"\n";

uint32_t g_text = 0;  // a guest buffer for our text
constexpr uint32_t kTextBytes = 2048;
uint32_t g_modern_label = 0;
bool g_opening = false;  // the screen is opening: its choice is not the player's

bool Modern() { return game::Opt().modern_layout; }

uint32_t GuestText(const std::string& text) {
    if (!g_text) g_text = mem::AllocSystem(kTextBytes);
    if (!g_text || text.size() >= kTextBytes) return 0;
    std::memcpy(kernel::HostPtr(g_text), text.c_str(), text.size() + 1);
    return g_text;
}

bool IsStyleExec(uint32_t text) {
    static constexpr char kExec[] = "exec style_";
    return text && std::memcmp(kernel::HostPtr(text), kExec, sizeof(kExec) - 1) == 0;
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_002AABB0);
X86_EXTERN_FUNC(__imp__sub_00335D60);
X86_EXTERN_FUNC(__imp__sub_002B3450);

// the first key bound to a command (edi), or -1: the hints' [STANCE_DOWN] asks for "lowerstance", which Modern does
// on B by code, so B (CIRCLE, key 0xD5 in the key name table at 0x8D32F0) is its button there
X86_FUNC(sub_002B3450) {
    uint32_t command = ctx.edi;
    __imp__sub_002B3450(ctx, base);
    static constexpr char kLower[] = "lowerstance";
    if (int32_t(ctx.eax) == -1 && command && Modern() &&
        std::memcmp(kernel::HostPtr(command), kLower, sizeof(kLower)) == 0)
        ctx.eax = 0xD5;
}

// Cbuf_AddText (text in eax): a style's exec gets the Modern binds after it while Modern is picked
X86_FUNC(sub_002AABB0) {
    uint32_t text = ctx.eax;
    bool style = IsStyleExec(text) && Modern();
    __imp__sub_002AABB0(ctx, base);
    if (!style) return;
    // the buffer is ours until Cbuf_Execute copies it: the text is added, not kept by pointer
    uint32_t ours = GuestText(kModernBinds);
    if (!ours) return;
    uint32_t eax = ctx.eax;
    ctx.eax = ours;
    kernel::CallGuest(ctx, kCbufAddText, {});  // (the original's ret already went: a call of its own)
    ctx.eax = eax;
    W32(kChoice, kModern);
}

// the Controller screen's choice shown and applied (eax screen); a change by the player is kept as Modern or not
X86_FUNC(sub_00335D60) {
    uint32_t screen = ctx.eax;
    if (!g_opening) {
        bool modern = int32_t(R32(screen + 0xC0)) == kModern;
        if (modern != Modern()) {
            game::Opt().modern_layout = modern;
            app::Save();
            gpu::texture_override::Changed();  // the Controller screen's picture (src/game/pad_picture.cpp)
            LOGR("controller configuration: %s", modern ? "Modern" : "the game's own");
        }
    } else if (Modern()) {
        W32(screen + 0xC0, kModern);
    }
    g_opening = false;
    __imp__sub_00335D60(ctx, base);
    if (int32_t(R32(screen + 0xC0)) != kModern) return;
    // the letter: "Modern"
    uint32_t label = R32(screen + 0x44);
    if (!g_modern_label) {
        g_modern_label = mem::AllocSystem(8);
        if (g_modern_label) std::memcpy(kernel::HostPtr(g_modern_label), "Modern", 7);
    }
    if (!label || !g_modern_label) return;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = label;
    kernel::CallGuest(ctx, R32(R32(label) + 0x78), {g_modern_label});
    ctx.ecx = ecx;
}

// 0x336118 "cmp ecx, eax" with eax 3: the choice may step to 4
void ControllerChoiceMax(X86Context& ctx, uint8_t*) { ctx.eax = kModern; }

// 0x338FD5 "cmp eax, 4" opening the screen: true keeps the choice (jumps past the reset to 0). The screen shows Modern
// only when Modern is what plays: a 4 left in the profile's cg_controllerconfig with Modern off is the game's A
// (style_h, as the game plays it), and is shown as A.
bool ControllerChoiceOpen(X86Context& ctx, uint8_t*) {
    g_opening = true;
    LOGR("controller configuration on opening: %s", Modern() ? "Modern" : "the game's own");
    if (Modern()) {
        ctx.eax = kModern;
        W32(kChoice, kModern);
    }
    return Modern() || int32_t(ctx.eax) != kModern;  // false: the game's own code sets 0 (A)
}

// 0x332FB1, the profile loaded, its cg_controllerconfig (eax) about to pick a style: 1-3 add "exec style_e/f/g.cfg"
// as the Controller screen does, but 0 copies "exec style_h.cfg" straight into the command buffer and 4 adds
// nothing, so the Modern binds never followed. With Modern picked the exec is added here, and the binds with it
// (sub_002AABB0); true skips to the buffer's execution (0x333037)
bool ControllerConfigLoaded(X86Context& ctx, uint8_t*) {
    if (!Modern()) return false;
    constexpr uint32_t kExecStyleH = 0x82DFF4;  // the game's "exec style_h.cfg\n"
    uint32_t eax = ctx.eax;
    ctx.eax = kExecStyleH;
    kernel::CallGuest(ctx, kCbufAddText, {});
    ctx.eax = eax;
    LOGR("controller configuration loaded: Modern");
    return true;
}

// 0x3362E5, Reset to Defaults just set the choice (edi screen) to 0: the default is Modern
void ControllerChoiceDefault(X86Context& ctx, uint8_t*) { W32(ctx.edi + 0xC0, kModern); }

// The look sensitivities (the profile's +0x44 and +0x48: 0x8DCE74, 0x8DCE78) are set by the Controller screen's
// Apply (item 6 of 0x3361C0) and the pause menu's Options (0x331340), but the game writes its profile only now and
// then (making it, finishing a mission): a change made in a menu was gone after quitting. Each one is kept in the
// settings file, and put in the profile whenever the game applies a profile (0x332E20, edi the profile).
constexpr uint32_t kLookX = 0x8DCE74, kLookY = 0x8DCE78;

void KeepLookSensitivity() {
    int32_t x = int32_t(R32(kLookX)), y = int32_t(R32(kLookY));
    game::Options& o = game::Opt();
    if (x == o.look_sensitivity_x && y == o.look_sensitivity_y) return;
    o.look_sensitivity_x = x, o.look_sensitivity_y = y;
    app::Save();
    LOGR("look sensitivity kept: %d, %d", x, y);
}

X86_EXTERN_FUNC(__imp__sub_003361C0);
X86_EXTERN_FUNC(__imp__sub_00331340);
X86_EXTERN_FUNC(__imp__sub_00332E20);

// the Controller screen's choice of a line (ecx screen, the line on the stack): 6 is Apply
X86_FUNC(sub_003361C0) {
    bool apply = R32(ctx.esp + 4) == 6;
    __imp__sub_003361C0(ctx, base);
    if (apply) KeepLookSensitivity();
}

// the pause menu's Options applied (esi screen)
X86_FUNC(sub_00331340) {
    __imp__sub_00331340(ctx, base);
    KeepLookSensitivity();
}

// a profile applied (edi the profile: its look sensitivities at +0x44, +0x48)
X86_FUNC(sub_00332E20) {
    const game::Options& o = game::Opt();
    if (ctx.edi && o.look_sensitivity_x >= 0 && o.look_sensitivity_y >= 0) {
        W32(ctx.edi + 0x44, uint32_t(o.look_sensitivity_x));
        W32(ctx.edi + 0x48, uint32_t(o.look_sensitivity_y));
    }
    __imp__sub_00332E20(ctx, base);
}

namespace game {

// each poll of port 0's controller
void ModernPad(input::Pad& pad) {
    if (!Modern()) return;
    // B: crouch or stand on a tap, prone when held
    constexpr uint8_t kPressed = 0x40;
    constexpr int64_t kHoldMs = 350;
    static bool down = false, fired = false;
    static int64_t since = 0;
    bool now = pad.connected && pad.analog[1] > kPressed && input::Playing();
    int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now && !down) {
        since = ms;
        fired = false;
    } else if (now && !fired && ms - since >= kHoldMs) {
        input::QueueCommand("goprone");
        fired = true;
    } else if (!now && down && !fired) {
        input::QueueCommand("togglecrouch");
    }
    down = now;
}

// each client frame: the queued console commands
void RunCommands(X86Context& ctx) {
    cheats::Tick(ctx);
    std::string command;
    while (input::TakeCommand(command)) {
        if (kernel::R32(0x95F8F8)) continue;  // the front end: no player
        // the Hide HUD key (src/common/mouse_keyboard.cpp): the Gameplay menu's setting, off and on
        if (command == "pc_hidehud") {
            Opt().hide_hud = !Opt().hide_hud;
            app::Save();
            LOGR("hide the HUD: %s", Opt().hide_hud ? "on" : "off");
            continue;
        }
        uint32_t text = GuestText(command + "\n");
        if (!text) continue;
        uint32_t eax = ctx.eax;
        ctx.eax = text;
        kernel::CallGuest(ctx, kCbufAddText, {});
        ctx.eax = eax;
    }
}

}  // namespace game
