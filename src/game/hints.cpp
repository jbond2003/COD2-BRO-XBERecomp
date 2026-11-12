// Tutorial hints on the keyboard and mouse. The training hints are built from tokens ("[USE_LEFT_STICK] to move
// around") whose texts are entries of the global string table (sp/global.cod, loaded with the front end): loaded,
// an entry is a pointer to its text, a pointer to a "[text]" copy, the text padded with zeros and the copy. The button tokens ("Press ~cross") already show keycaps
// (prompts.cpp); the stick ones are words, so while the keyboard and mouse are in use those entries' texts are
// rewritten in place to the keys (within the room each entry has), and put back when a controller is used. The prone
// hint ("press crouch again") follows the new controls: Z on the keys, B held with the Modern layout.
//
// A hint on the screen when the device changes is given again in the new words (ShowAgain).
//
// Lines of our own in the first mission: sprint, 5 seconds after the game's "to move around" hint (the level
// script's, as the player gains control after the opening scene), and on the keyboard and mouse leaning 5 seconds
// after that and the Hide HUD key 5 seconds after leaning. Each waits until a hint of the game's own has gone, and one
// the game's hint cut short comes again (UpdateLines). The script's hint (0x2FEF80: the text's key hash) goes to the HUD (0x32CED0, esi the HUD [0x95F8F0], eax
// the hash): the text found in the global table is given to the hint widget ([HUD+0x3C], vtable +0x140 = 0x32EE50:
// text, 400, -1.0) and the HUD's timer at +0x60 restarted. Ours go the same way, so they look and move like the game's.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace {

struct Hint {
    const char* pad;  // the game's own text
    std::string (*keys)();
    std::string (*modern)() = nullptr;  // on a controller with the Modern layout, if not the game's own
    uint32_t at = 0;    // the text in guest memory, 0 until found
    uint32_t room = 0;  // bytes before its "[text]" copy
};

// Our lines, and the prone hints, name their keys with the game's button tokens, so the keys show as keycaps like the
// rest of the game's hints: two icons the hints do not use in play (~updown and ~both_lr, the dpad's up and down and
// its left and right) carry those keys while such a line is on the screen on the keyboard (game::HintKeys, read by
// prompts.cpp).
constexpr const char* kKeyTokens[] = {"~updown", "~both_lr"};
constexpr const char* kProneToken = "~both_lr";

std::string Key(const char* action, const char* fallback) {
    std::string k = input::KeyName(action);
    return k.empty() ? fallback : k;
}

std::string Move() {
    std::string f = Key("move_forward", "W"), l = Key("strafe_left", "A"), b = Key("move_back", "S"),
                r = Key("strafe_right", "D");
    if (f.size() == 1 && l.size() == 1 && b.size() == 1 && r.size() == 1) return "Use " + f + l + b + r;
    return "Use " + f + "/" + l + "/" + b + "/" + r;
}
std::string MoveLower() {
    std::string m = Move();
    m[0] = 'u';
    return m;
}
std::string Look() { return "Move the mouse"; }
std::string Sprint() { return "Press and hold " + Key("sprint", "Shift"); }
std::string Prone() { return std::string("Press ") + kProneToken + " to go prone"; }
std::string HoldProne() { return "Hold [%s] to go prone"; }  // B held, its picture as before
// the stance token draws the crouch key or button; on the keys prone has its own key, on Modern B is held
std::string CrouchProne() { return std::string("[PRESS_STANCE_DOWN] to crouch, ") + kProneToken + " to go prone"; }
std::string CrouchHold() { return "[PRESS_STANCE_DOWN] to crouch, hold it to go prone"; }

Hint g_hints[] = {
    {"Move the left thumbstick", Move},
    {"Use the left analog stick", Move},
    {"Move the right thumbstick", Look},
    {"Use the right analog stick", Look},
    {"the left thumbstick", MoveLower},
    {"the right thumbstick", [] { return std::string("the mouse"); }},
    {"Click and hold the left thumbstick", Sprint},
    {"Press [%s] to go prone", Prone, HoldProne},  // the original: the crouch button again
    {"[PRESS_STANCE_DOWN] to crouch, press it again to go prone", CrouchProne, CrouchHold},  // a level's own
    // the [PRESS_ATTACK] token's text ("Pull ~r2 to fire"): a mouse button is pressed, the picture as before
    {"Pull ~r2", [] { return std::string("Press ~r2"); }},
};

bool g_searched = false;
double g_scanned = 0;  // when, in seconds; a level's own texts load after the global ones
bool g_keyboard = false;
bool g_modern = false;
uint32_t g_version = ~0u;

int Found() {
    return int(std::count_if(std::begin(g_hints), std::end(g_hints), [](const Hint& h) { return h.at != 0; }));
}

// each hint's entry in the loaded string table: the header, the text, its "[text]" copy
void Find() {
    uint8_t* mem = static_cast<uint8_t*>(kernel::HostPtr(mem::kContiguousBase));
    size_t size = mem::kPhysicalSize;
    int found = 0;
    for (Hint& h : g_hints) {
        if (h.at) {
            found++;
            continue;
        }
        size_t len = std::strlen(h.pad);
        std::string want = std::string(h.pad) + '\0';
        for (size_t i = 12; i + want.size() < size; i++) {
            const void* p = std::memchr(mem + i, h.pad[0], size - i - want.size());
            if (!p) break;
            i = size_t(static_cast<const uint8_t*>(p) - mem);
            if (std::memcmp(mem + i, want.data(), want.size()) != 0) continue;
            // loaded, the entry starts with two pointers: to its text (this) and to its "[text]" copy
            uint32_t va = mem::kContiguousBase + uint32_t(i);
            uint32_t self = kernel::R32(va - 8), copy = kernel::R32(va - 4);
            if (self != va || copy <= va + len || copy > va + len + 8) continue;
            uint32_t room = copy - va;
            h.at = mem::kContiguousBase + uint32_t(i);
            h.room = room;
            found++;
            break;
        }
    }
    LOGR("hints: %d of %zu tutorial texts found", found, std::size(g_hints));
}

void Write(const Hint& h, const std::string& text) {
    std::string t = text;
    // no room for the token: the key's name in words
    if (size_t at = t.find(kProneToken); t.size() >= h.room && at != std::string::npos)
        t.replace(at, std::strlen(kProneToken), Key("prone", "Z"));
    if (t.size() >= h.room) t = t.substr(0, h.room - 1);
    uint8_t* p = static_cast<uint8_t*>(kernel::HostPtr(h.at));
    std::memset(p, 0, h.room);
    std::memcpy(p, t.data(), t.size());
}

// --- our own hint lines

constexpr uint32_t kHud = 0x95F8F0;           // the in-game HUD object; its hint widget at +0x3C, hint timer at +0x60
constexpr uint32_t kGlobalTable = 0x95F5D8;   // the global string table
constexpr uint32_t kLookup = 0x2984F0;        // (eax table; the key's hash on the stack) the entry, its text at +8
constexpr uint32_t kSetHint = 0x32EE50;       // hint widget's (ecx) text, with the game's reveal and duration
constexpr uint32_t kSetHintArgs[] = {0x190, 0xBF800000u};
constexpr double kSprintAfter = 5.0;  // seconds after the game's move hint

double g_first = 0;  // when the game's move hint was set, 0 outside the sequence
int g_lines = 0;     // our lines shown since
double g_next = 0;   // our next line not before this
double g_line_at = 0;  // when our last line was put up
bool g_prone_up = false;  // the hint on the screen names the prone key by its token
uint32_t g_text = 0;
uint32_t g_shown = 0;  // a tutorial text the HUD is given in our words
uint32_t g_hash = 0xFFFFFFFFu;  // the script's hint on the screen, by its key's hash; none
int g_own = 0;                  // or ours: 1 sprint, 2 leaning, 3 the Hide HUD key
bool g_again = false;           // the hint on the screen is being given again, in the other device's words

bool InLevel() { return !kernel::R32(0x95F8F8) && kernel::R32(0x95F8FC); }

double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }


std::string Lean() { return std::string("Press ") + kKeyTokens[0] + " and " + kKeyTokens[1] + " to lean"; }

void ShowLine(X86Context& ctx, const std::string& text) {
    constexpr uint32_t kBytes = 256;
    if (!g_text) g_text = mem::AllocSystem(kBytes);
    uint32_t hud = kernel::R32(kHud);
    uint32_t widget = hud ? kernel::R32(hud + 0x3C) : 0;
    if (!g_text || !widget || text.size() >= kBytes) return;
    std::memcpy(kernel::HostPtr(g_text), text.c_str(), text.size() + 1);
    uint32_t ecx = ctx.ecx;
    ctx.ecx = widget;
    kernel::CallGuest(ctx, kSetHint, {g_text, kSetHintArgs[0], kSetHintArgs[1]});
    ctx.ecx = ecx;
    if (!g_again) kernel::W32(hud + 0x60, 0);
    LOGR("hint line: \"%s\"", text.c_str());
}

std::string SprintLine() {
    // the token is the game's, "L3 Button" on a controller
    return g_keyboard ? std::string("Hold ") + kKeyTokens[0] + " to sprint" : std::string("Hold the [SPRINT] to sprint");
}

// our lines, in their time after the game's move hint (each client frame): each waits for its time and for the
// game's own hint to have gone (the crouch hint came just as the sprint line went up and took its place, and the
// others never came), at most kWaitMost
constexpr double kGap = 5.0, kWaitMost = 15.0;

bool GameHintUp() {
    uint32_t hud = kernel::R32(kHud);
    return hud && !(kernel::R32(hud + 0x60) & 0x80000000u) && !g_own && g_hash != 0xFFFFFFFFu;
}

void UpdateLines(X86Context& ctx) {
    if (!g_first) return;
    if (!InLevel() || !game::Opt().hints || game::Opt().hide_hud) {
        g_first = 0;
        return;
    }
    double now = Now();
    if (now < g_next) return;
    if (GameHintUp() && now < g_next + kWaitMost) return;
    // the lines left out on a controller (leaning and Hide HUD are keys)
    while (g_lines > 0 && g_lines < 3 && !g_keyboard) g_lines++;
    if (g_lines >= 3) {
        g_first = 0;
        return;
    }
    if (g_lines == 0) ShowLine(ctx, SprintLine());
    else if (g_lines == 1) ShowLine(ctx, Lean());
    else ShowLine(ctx, std::string("Press ") + kKeyTokens[0] + " to hide the HUD");
    g_lines++;
    g_own = g_lines;
    g_line_at = now;
    g_next = now + kGap;
}

// hints switched off, or the HUD hidden, with a hint on the screen: it goes at once, the way the script takes one
// away (0x32CED0 with no hash: an empty text, the HUD's timer at its end)
void ClearShown(X86Context& ctx) {
    static bool was_off = false;
    bool off = !game::Opt().hints || game::Opt().hide_hud;
    bool now_off = off && !was_off;
    was_off = off;
    uint32_t hud = kernel::R32(kHud);
    if (!now_off || !InLevel() || !hud || !kernel::R32(hud + 0x3C)) return;
    uint32_t esi = ctx.esi, eax = ctx.eax;
    ctx.esi = hud;
    ctx.eax = 0xFFFFFFFFu;
    kernel::CallGuest(ctx, 0x32CED0, {});
    ctx.esi = esi;
    ctx.eax = eax;
    LOGR("hint cleared");
}

// The device, the bindings or the layout changed with a hint on the screen: the hint was made from the texts as
// they were, so it is given again from the texts as they are now ("Press" a mouse button, "Pull" a trigger). The
// HUD's timer keeps its place, so the hint does not start over.
void ShowAgain(X86Context& ctx) {
    uint32_t hud = kernel::R32(kHud);
    if (!InLevel() || !hud || !kernel::R32(hud + 0x3C)) return;
    uint32_t timer = kernel::R32(hud + 0x60);
    if (timer & 0x80000000u) {  // -1.0: no hint
        g_hash = 0xFFFFFFFFu;
        g_own = 0;
        return;
    }
    if (g_hash == 0xFFFFFFFFu && !g_own) return;
    g_again = true;
    if (g_own == 1) {
        ShowLine(ctx, SprintLine());
    } else {
        // the script's hint again; a line of ours for the keys only goes when a controller is taken up
        uint32_t esi = ctx.esi, eax = ctx.eax;
        ctx.esi = hud;
        ctx.eax = g_own ? 0xFFFFFFFFu : g_hash;
        if (!g_own || !g_keyboard) {
            kernel::CallGuest(ctx, 0x32CED0, {});
            LOGR("hint given again for the %s", g_keyboard ? "keyboard and mouse" : "controller");
        }
        ctx.esi = esi;
        ctx.eax = eax;
    }
    g_again = false;
    if (!(kernel::R32(hud + 0x60) & 0x80000000u)) kernel::W32(hud + 0x60, timer);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_0032CED0);

// the HUD's hint from the level script (esi the HUD, eax the text's key hash, -1 none): the game's move hint starts
// our lines, any other ends them
X86_FUNC(sub_0032CED0) {
    // tutorial hints switched off, or the HUD hidden (the Gameplay menu): the script's hint is no hint
    if (!game::Opt().hints || game::Opt().hide_hud) ctx.eax = 0xFFFFFFFFu;
    uint32_t hash = ctx.eax;
    if (!g_again) {
        // a game hint over our line: the line comes again after it, if it was up for less than 3 seconds
        if (g_own && g_first && Now() - g_line_at < 3.0 && hash != 0xFFFFFFFFu) g_lines = g_own - 1;
        if (g_first) g_next = std::max(g_next, Now() + kGap);
        g_hash = hash;
        g_own = 0;
    } else if (hash == 0xFFFFFFFFu) {
        g_own = 0;
    }
    uint32_t entry = 0, text = 0;
    if (hash != 0xFFFFFFFFu) {
        ctx.eax = kernel::R32(kGlobalTable);
        entry = kernel::CallGuest(ctx, kLookup, {hash});
        ctx.eax = hash;
        text = entry ? kernel::R32(entry + 8) : 0;
        if (text < mem::kContiguousBase) text = 0;
    }
    // one of the tutorial texts still in the game's words (a copy the scan did not reach): shown in ours, the entry
    // pointed at our text for the call and put back after
    uint32_t shown = 0;
    if (text) {
        const char* t = static_cast<const char*>(kernel::HostPtr(text));
        for (const Hint& h : g_hints) {
            if (std::strcmp(t, h.pad) != 0) continue;
            std::string want = g_keyboard ? h.keys() : g_modern && h.modern ? h.modern() : std::string();
            if (want.empty() || want.size() >= 256) break;
            if (!g_shown) g_shown = mem::AllocSystem(256);
            if (!g_shown) break;
            std::memcpy(kernel::HostPtr(g_shown), want.c_str(), want.size() + 1);
            shown = g_shown;
            kernel::W32(entry + 8, shown);
            LOGR("hint shown as \"%s\"", want.c_str());
            break;
        }
    }
    if (!g_again || hash == 0xFFFFFFFFu) {
        uint32_t t = shown ? shown : text;
        g_prone_up = t && std::strstr(static_cast<const char*>(kernel::HostPtr(t)), kProneToken);
    }
    __imp__sub_0032CED0(ctx, base);
    if (shown) kernel::W32(entry + 8, text);
    if (!g_again && text && std::strstr(static_cast<const char*>(kernel::HostPtr(text)), "to move around")) {
        g_first = Now();
        g_lines = 0;
        g_next = g_first + kSprintAfter;
    }
}

namespace game {

// each client frame
void UpdateHints(X86Context& ctx) {
    UpdateLines(ctx);
    ClearShown(ctx);
    uint32_t mode = Opt().button_prompts;
    bool keyboard = mode == 1 || (mode != 2 && input::KeyboardActive());
    uint32_t version = input::BindingsVersion();
    // the tables load with the front end and each level; the hints are only shown in a level (no front end manager).
    // Each level is searched again for the texts not found yet, a few seconds apart while it loads
    if (kernel::R32(0x95F8F8) || !kernel::R32(0x95F8FC)) {
        g_searched = false;
        return;
    }
    // a found text whose entry is gone (the next mission loaded its own table without the front end between): looked
    // for again, never written to
    for (Hint& h : g_hints)
        if (h.at && (kernel::R32(h.at - 8) != h.at || std::strlen(static_cast<const char*>(kernel::HostPtr(h.at))) >= h.room))
            h.at = 0;
    bool all = std::all_of(std::begin(g_hints), std::end(g_hints), [](const Hint& h) { return h.at != 0; });
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!g_searched || (!all && now - g_scanned >= 3.0)) {
        g_scanned = now;
        int before = Found();
        Find();
        // the texts found now are written below, whatever the mode was (a level's own load late: the first scan misses them)
        if (!g_searched || Found() != before) g_version = ~0u;
        g_searched = true;
    }
    bool modern = Opt().modern_layout;
    if (keyboard == g_keyboard && version == g_version && modern == g_modern) return;
    g_keyboard = keyboard;
    g_version = version;
    g_modern = modern;
    for (const Hint& h : g_hints) {
        if (!h.at) continue;
        if (keyboard) Write(h, h.keys());
        else Write(h, modern && h.modern ? h.modern() : std::string(h.pad));
    }
    ShowAgain(ctx);
}

}  // namespace game

namespace game {

// the actions whose keys the hint on the screen shows, for its two key tokens (kKeyTokens): none (null) when
// the line is not ours or a controller is in use
void HintKeys(const char* (&actions)[2]) {
    actions[0] = actions[1] = nullptr;
    if (!g_keyboard) return;
    if (g_own == 1) actions[0] = "sprint";
    if (g_own == 2) actions[0] = "lean_left", actions[1] = "lean_right";
    if (g_own == 3) actions[0] = "hide_hud";
    if (!g_own && g_prone_up) actions[1] = "prone";
}

}  // namespace game
