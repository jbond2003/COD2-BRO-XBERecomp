// The mouse in the game's menus, and typing on the profile name typewriter.
//
// The UI is run by managers: front end [0x95F8F8] (null in a level), in game [0x95F8FC] (the pause menu; the HUD is
// a screen without items), dialogs [0x95F900]. Each has screens (+4 array, +0xC the active one) that are menus of
// items. Every frame each manager polls the pad (0x31FC10, ecx manager) and sends the first new press to its active
// screen (dpad moves the selection, A activates it).
//   screen: +4 items, +0x22 selected index (int16), +0x26 item count (int16), +0x30 locked (animating), +0x31 plays
//           the move sound; vtable +0x10 the move sound, +0x88 select(index, 1) (thiscall)
//   item:   +8/+0xA/+0xC/+0xE the neighbour up/down/left/right (int16, -1 none), +0x10 its text widget, +0x16 disabled
//   text widget (vtable 0x8298B8, draw 0x321160): +0x10 bit 2 visible, +0x1C its string (text at +6), +0x2C x and
//           +0x30 y in 640x480, +0x5C alignment (0x10 left, 0x20 right, else centred; 0x40 top, 0x80 bottom);
//           width 0x321090, height 0x321030 (thiscall, float in st0)
// The layout above and the pointer behaviour follow CoD2-BRO-Native-Loader (GPL-2.0-or-later, see
// THIRD_PARTY_NOTICES.md): hovering selects an item, a left click selects it and taps A, a right click taps B, the
// wheel taps up/down. An option row ("Invert Aim  < off >", a slider) takes clicks right of its label: left or right
// of the value's middle taps left/right. Pickers whose items are only linked left/right (difficulty) take clicks
// left/right of the middle.
//
// The typewriter (screen vtable 0x82A068, Create Profile): 40 keys, 0-9 digits, 10-35 a to z in rows of 10, 9, 9, 8,
// 36 backspace, 37 shift, 38 space, 39 done. Its activate (0x338C40, thiscall index) types index through 0x325980
// (eax index, esi screen): the letter comes from the key's index, not its label. The keys' caps are the picture
// behind, so the QWERTY layout relabels the keys and maps each key to its new letter in 0x325980.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "common/menu.h"
#include "common/menu_pointer.h"
#include "common/menu_state.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;
using kernel::W32;

namespace {

constexpr uint32_t kFrontEnd = 0x95F8F8, kInGame = 0x95F8FC, kDialogs = 0x95F900;
constexpr uint32_t kEngineFrame = 0x9028E0;
constexpr uint32_t kTextWidgetVtable = 0x8298B8;
constexpr uint32_t kWidgetWidth = 0x321090, kWidgetHeight = 0x321030;
constexpr uint32_t kSoundSlot = 0x10, kSelectSlot = 0x88;
constexpr uint32_t kPressStartVtable = 0x8295A0;  // in game, after a level loads: "Press A to start"
constexpr uint32_t kTypewriterVtable = 0x82A068;
constexpr uint32_t kProfilesVtable = 0x828ED0;  // Profile Manager
constexpr uint32_t kBonusVtable = 0x82A708;     // Bonus Materials
constexpr uint32_t kTypewriterActivate = 0x338C40;
constexpr int16_t kSpaceKey = 38;

int16_t R16s(uint32_t a) { return int16_t(kernel::G<uint16_t>(a)); }
uint8_t R8(uint32_t a) { return kernel::G<uint8_t>(a); }
float RF(uint32_t a) { return kernel::G<float>(a); }
uint32_t Frame() { return R32(kEngineFrame); }

std::mutex g_drawn_lock;
std::unordered_map<uint32_t, uint32_t> g_drawn;  // text widget -> engine frame of its last draw
uint32_t g_dialog_frame = 0;
bool g_dialog_shown = false;
uint32_t g_pointer_frame = 0;  // a manager last let the mouse point
bool g_cursor = false;

bool DrawnLately(uint32_t widget, uint32_t frame) {
    std::lock_guard lock(g_drawn_lock);
    auto it = g_drawn.find(widget);
    return it != g_drawn.end() && frame - it->second <= 2;  // the poll runs before this frame's draws
}

float WidgetMethod(X86Context& ctx, uint32_t fn, uint32_t widget) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = widget;
    kernel::CallGuest(ctx, fn, {});
    ctx.ecx = ecx;
    float v = float(X87_ST(0));
    x87_pop(ctx);
    return v;
}

struct ItemBox {
    int16_t index = -1;
    game::menu::Rect box;
};

// a text widget's box in 640x480, with a little around the letters; false without text
bool WidgetBox(X86Context& ctx, uint32_t widget, game::menu::Rect& box) {
    float w = WidgetMethod(ctx, kWidgetWidth, widget), h = WidgetMethod(ctx, kWidgetHeight, widget);
    if (!(w > 0 && h > 0) || !std::isfinite(w) || !std::isfinite(h)) return false;
    uint8_t align = R8(widget + 0x5C);
    float left = RF(widget + 0x2C), top = RF(widget + 0x30);
    left -= (align & 0x20) ? w : (align & 0x10) ? 0.0f : w / 2;
    top -= (align & 0x80) ? h : (align & 0x40) ? 0.0f : h / 2;
    constexpr float kMargin = 3.0f;
    box = {left - kMargin, top - kMargin, left + w + kMargin, top + h + kMargin};
    return box.Valid();
}

// the active screen's enabled items on screen, with their text's box
std::vector<ItemBox> ShownItems(X86Context& ctx, uint32_t menu) {
    std::vector<ItemBox> out;
    uint32_t frame = Frame(), items = R32(menu + 4);
    int16_t count = R16s(menu + 0x26);
    for (int16_t i = 0; items && i < count; i++) {
        uint32_t item = R32(items + uint32_t(i) * 4);
        if (!item || R8(item + 0x16)) continue;
        uint32_t widget = R32(item + 0x10);
        if (!widget || R32(widget) != kTextWidgetVtable || !DrawnLately(widget, frame)) continue;
        ItemBox b;
        b.index = i;
        if (!WidgetBox(ctx, widget, b.box)) {
            // a picture without text (the typewriter's backspace, shift, space bar and done): around its place
            float x = RF(widget + 0x2C), y = RF(widget + 0x30);
            bool bar = R32(menu) == kTypewriterVtable && i == kSpaceKey;
            float half_w = bar ? 150.0f : 24.0f, half_h = bar ? 16.0f : 22.0f;
            b.box = {x - half_w, y - half_h, x + half_w, y + half_h};
        }
        if (b.box.Valid()) out.push_back(b);
    }
    return out;
}

// the x of an option row's value: the middle of the texts drawn on its line right of its label, else that of the
// other rows in the same column (sliders draw no text), else halfway to the right edge for a label in the left
// half; negative for none
float ValueMiddle(X86Context& ctx, const std::vector<ItemBox>& items, const ItemBox& row) {
    std::vector<uint32_t> drawn;
    uint32_t frame = Frame();
    {
        std::lock_guard lock(g_drawn_lock);
        for (const auto& [widget, at] : g_drawn)
            if (frame - at <= 2) drawn.push_back(widget);
    }
    float own = -1, others = 0;
    int other_count = 0;
    for (const ItemBox& item : items) {
        if (item.index != row.index && std::abs(item.box.left - row.box.left) >= 4.0f) continue;
        float middle = (item.box.top + item.box.bottom) / 2;
        float left = 1e9f, right = -1e9f;
        for (uint32_t widget : drawn) {
            game::menu::Rect text;
            if (R32(widget) != kTextWidgetVtable || !WidgetBox(ctx, widget, text)) continue;
            float text_middle = (text.top + text.bottom) / 2;
            if (std::abs(text_middle - middle) > (item.box.bottom - item.box.top) / 2 || text.left < item.box.right)
                continue;
            left = std::min(left, text.left);
            right = std::max(right, text.right);
        }
        if (right < left) continue;
        if (item.index == row.index) own = (left + right) / 2;
        else others += (left + right) / 2, other_count++;
    }
    if (own >= 0) return own;
    if (other_count) return others / float(other_count);
    if (row.box.right > 320.0f) return -1;  // a menu item, not an option's label
    return (row.box.right + 640.0f) / 2;
}

// items linked only left/right: a picker
bool Horizontal(uint32_t menu) {
    bool sideways = false;
    uint32_t items = R32(menu + 4);
    for (int16_t i = 0; items && i < R16s(menu + 0x26); i++) {
        uint32_t item = R32(items + uint32_t(i) * 4);
        if (!item) continue;
        if (R16s(item + 8) >= 0 || R16s(item + 0xA) >= 0) return false;
        sideways = sideways || R16s(item + 0xC) >= 0 || R16s(item + 0xE) >= 0;
    }
    return sideways;
}

constexpr uint16_t kDpadUp = 1, kDpadDown = 2, kDpadLeft = 4, kDpadRight = 8;
void TapA() { input::MenuTap(0, 1); }
void TapB() { input::MenuTap(0, 2); }
void TapDpad(uint16_t bits) { input::MenuTap(bits, 0); }

void Select(X86Context& ctx, uint32_t menu, int16_t index) {
    uint32_t vtable = R32(menu), ecx = ctx.ecx;
    ctx.ecx = menu;
    if (R8(menu + 0x31)) kernel::CallGuest(ctx, R32(vtable + kSoundSlot), {});
    ctx.ecx = menu;
    kernel::CallGuest(ctx, R32(vtable + kSelectSlot), {uint32_t(int32_t(index)), 1});
    ctx.ecx = ecx;
}

// ---- the typewriter ----------------------------------------------------------------------------------------------

constexpr int kKeys = 36, kBackspace = 36, kSpace = 38, kDone = 39;
// the character each key types in the QWERTY layout, by key: digits 1 to 0, then rows of 9, 9 and 8 (P has no room in
// the top row and sits at the end of the bottom one, beside Done)
constexpr char kQwerty[kKeys + 1] = "1234567890qwertyuioasdfghjklzxcvbnmp";
constexpr char kAbc[kKeys + 1] = "0123456789abcdefghijklmnopqrstuvwxyz";

bool Qwerty() { return game::Opt().qwerty_typewriter; }
char KeyChar(int key) { return (Qwerty() ? kQwerty : kAbc)[key]; }
// the game's index for a character: what 0x325980 turns into it
int GameIndex(char c) {
    const char* at = std::strchr(kAbc, c);
    return at && *at ? int(at - kAbc) : -1;
}
// the key showing a character
int KeyOf(char c) {
    for (int k = 0; k < kKeys; k++)
        if (KeyChar(k) == c) return k;
    return -1;
}

uint32_t g_typewriter = 0;  // the screen while it is up

// the keys' labels to the layout: each label string moves to the key that types its character, whatever case the
// shift key last gave them
void Relabel(uint32_t menu) {
    uint32_t items = R32(menu + 4);
    if (!items || R16s(menu + 0x26) < kKeys) return;
    uint32_t widget[kKeys], text[kKeys];
    int game_index[kKeys];
    for (int k = 0; k < kKeys; k++) {
        uint32_t item = R32(items + uint32_t(k) * 4);
        widget[k] = item ? R32(item + 0x10) : 0;
        if (!widget[k] || R32(widget[k]) != kTextWidgetVtable) return;
        text[k] = R32(widget[k] + 0x1C);
        char c = text[k] ? char(R8(text[k] + 6)) : 0;
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        game_index[k] = GameIndex(c);
        if (game_index[k] < 0) return;
    }
    uint32_t by_index[kKeys] = {};
    for (int k = 0; k < kKeys; k++) by_index[game_index[k]] = text[k];
    for (int k = 0; k < kKeys; k++) {
        uint32_t want = by_index[GameIndex(KeyChar(k))];
        if (want && text[k] != want) W32(widget[k] + 0x1C, want);
    }
}

// a typed character: the key showing it is selected and activated as an A press on it would, upper case through the
// screen's shift flag (+0x1A8) for that character alone
void Type(X86Context& ctx, uint32_t menu, char c) {
    int key = -1;
    if (c == '\b') key = kBackspace;
    else if (c == '\r') key = kDone;
    else if (c == ' ') key = kSpace;
    else key = KeyOf(char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    if (key < 0) return;
    uint32_t items = R32(menu + 4);
    uint32_t item = items ? R32(items + uint32_t(key) * 4) : 0;
    if (!item || R8(item + 0x16)) return;  // backspace with nothing typed
    if (R16s(menu + 0x22) != key) Select(ctx, menu, int16_t(key));
    uint8_t shift = R8(menu + 0x1A8);
    bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (letter) kernel::G<uint8_t>(menu + 0x1A8) = c >= 'A' && c <= 'Z';
    uint32_t ecx = ctx.ecx;
    ctx.ecx = menu;
    kernel::CallGuest(ctx, kTypewriterActivate, {uint32_t(key)});
    ctx.ecx = ecx;
    if (letter) kernel::G<uint8_t>(menu + 0x1A8) = shift;
}

void Typewriter(X86Context& ctx, uint32_t menu) {
    bool up = menu && R32(menu) == kTypewriterVtable;
    if (up != (g_typewriter != 0)) input::SetTextEntry(up);
    g_typewriter = up ? menu : 0;
    if (!up) return;
    Relabel(menu);
    if (R8(menu + 0x30)) return;  // locked
    if (char c = input::TakeTyped()) Type(ctx, menu, c);
}

// ---- the pointer ---------------------------------------------------------------------------------------------------

struct Pointer {
    bool inside = false, moved = false;
    float x = 0, y = 0;
    int left_clicks = 0, right_clicks = 0, wheel = 0;
};

Pointer TakePointer() {
    static float last_x = -1, last_y = -1;
    input::MenuPointer p = input::ReadMenuPointer();
    Pointer out;
    out.left_clicks = (p.pressed & 1) ? 1 : 0;
    out.right_clicks = (p.pressed & 2) ? 1 : 0;
    out.wheel = p.wheel;
    if (!p.active) return out;
    float x, y;
    if (!game::menu::MapPointer(p.x, p.y, p.width, p.height, 640, 480, app::Widescreen(), x, y)) return out;
    out.inside = true;
    out.x = x, out.y = y;
    out.moved = std::abs(x - last_x) > 0.5f || std::abs(y - last_y) > 0.5f;
    last_x = x, last_y = y;
    if (out.moved || out.left_clicks || out.right_clicks || out.wheel) input::UseMenuMouse();
    return out;
}

// ---- strings and PC Options --------------------------------------------------------------------------------------

// a game string: u16 capacity, u16 length, u16 references, then the text
bool SameText(uint32_t str, const char* want) {
    for (uint32_t i = 0;; i++) {
        char c = char(R8(str + 6 + i));
        if (c != want[i]) return false;
        if (!c) return true;
    }
}

// a string of ours the game can draw; never freed (its reference count stays above the game's)
uint32_t GameString(const char* text) {
    uint32_t len = uint32_t(std::strlen(text));
    uint32_t at = mem::AllocSystem(8 + len + 1);
    if (!at) return 0;
    kernel::G<uint16_t>(at) = uint16_t(len + 1);
    kernel::G<uint16_t>(at + 2) = uint16_t(len);
    kernel::G<uint16_t>(at + 4) = 1;
    for (uint32_t i = 0; i <= len; i++) kernel::G<uint8_t>(at + 6 + i) = uint8_t(text[i]);
    return at;
}

// The Options menu (screen vtable 0x829348: GAMEPLAY, CONTROLLER, SOUND) gets a fourth line, PC OPTIONS, which opens
// the PC settings panel. It is not one of the screen's items: a copy of the last item's text widget (0x68 bytes) is
// drawn one line further down right after it, and our selection of it is kept here. While it is selected the game's
// own selection stays on SOUND, drawn unselected (colour +0x50, alpha 0xFF selected, 0x8C not), and the pad's
// up/down/A go to it instead of the screen (OptionsPad).
constexpr uint32_t kOptionsVtable = 0x829348;
constexpr uint32_t kWidgetBytes = 0x68;
constexpr uint32_t kSelectedAlpha = 0xFF000000, kIdleAlpha = 0x8C000000;
struct PcEntry {
    uint32_t screen = 0;  // the Options screen it was made for
    uint32_t anchor = 0;  // the last item's widget, SOUND
    uint32_t widget = 0;  // our copy, saying PC OPTIONS
    uint32_t frame = 0;   // the last frame the Options screen was up
    bool selected = false;
    bool sound = false;         // play the move sound at the next poll
    bool select_first = false;  // down from PC OPTIONS wraps to the first item
} g_pc;

// Menu lines that are gone: the main menu's Multiplayer (screen vtable 0x829B80: Single Player, Multiplayer,
// Options, Bonus Materials, and a hidden DISABLED item). The item is marked
// disabled (+0x16), which the screen's own up/down and the mouse skip, its text is not drawn, and the lines under it
// move up into its place. The multiplayer and Live code is left as it is.
constexpr uint32_t kMainMenuVtable = 0x829B80;
std::unordered_map<uint32_t, uint32_t> g_hidden;  // a hidden line's text widget -> engine frame last seen
std::unordered_map<uint32_t, float> g_moved_y;   // widget -> the y we gave it
std::unordered_map<uint32_t, float> g_original_y;  // widget -> the y the game gave it

bool Hidden(uint32_t widget) {
    std::lock_guard lock(g_drawn_lock);
    return g_hidden.count(widget) != 0;
}

int16_t g_main_gone = -1;  // the main menu's hidden line (Multiplayer)

void HideItem(X86Context& ctx, uint32_t menu, const char* label) {
    uint32_t items = R32(menu + 4);
    int16_t count = R16s(menu + 0x26);
    int16_t gone = -1;
    // each line's own place, as the game last laid it out (not the one we moved it to)
    auto original = [](uint32_t widget) {
        float y = RF(widget + 0x30);
        auto it = g_moved_y.find(widget);
        if (it != g_moved_y.end() && it->second == y) return g_original_y[widget];
        g_original_y[widget] = y;
        return y;
    };
    float above_y = NAN;  // the place of the line above, in the game's order
    for (int16_t i = 0; items && i < count; i++) {
        uint32_t item = R32(items + uint32_t(i) * 4), widget = item ? R32(item + 0x10) : 0;
        if (!widget || R32(widget) != kTextWidgetVtable) continue;
        uint32_t text = R32(widget + 0x1C);
        float own = original(widget);
        if (gone < 0 && text && SameText(text, label)) {
            gone = i;
            {
                std::lock_guard lock(g_drawn_lock);
                if (g_hidden.size() > 64) g_hidden.clear();
                g_hidden[widget] = Frame();
            }
            kernel::G<uint8_t>(item + 0x16) = 1;
            above_y = own;
            continue;
        }
        // a line under the hidden one takes the place of the line above it, so the lines keep the game's spacing
        float want = above_y;
        above_y = own;
        if (gone < 0 || R8(item + 0x16) || !std::isfinite(want) || !(own - want > 0 && own - want < 100)) continue;
        if (RF(widget + 0x30) != want) {
            if (g_moved_y.size() > 256) g_moved_y.clear(), g_original_y.clear();
            kernel::G<float>(widget + 0x30) = want;
        }
        g_moved_y[widget] = want;
    }
    if (R32(menu) == kMainMenuVtable) g_main_gone = gone;
    // never left on it (a screen made with it selected)
    if (gone >= 0 && R16s(menu + 0x22) == gone && !R8(menu + 0x30))
        Select(ctx, menu, int16_t(gone > 0 ? gone - 1 : gone + 1));
}

// The Xbox Live corner of the front end: "Not signed in" / "Sign-in failed" left, and right the Y "Xbox Live"
// button prompt (sub_0030F900) with an empty text widget under it whose button (+0x54) is "ybutton". None of it is
// drawn, and Y does nothing while it would be up (in OptionsPad).
uint32_t g_live_frame = 0;  // the corner was last up

bool XboxLiveText(uint32_t text) {
    static constexpr const char* kTexts[] = {"Not signed in", "Sign-in failed", "Signed in", "Signing in"};
    for (const char* t : kTexts)
        if (SameText(text, t)) return true;
    // the button picture's code, then the words
    // (the button picture's code first, and the length field is not kept: up to the capacity)
    uint32_t len = kernel::G<uint16_t>(text);
    const char* want = "Xbox Live";
    if (len < 9 || len > 64) return false;
    for (uint32_t at = 0; at + 9 <= len; at++) {
        bool same = true;
        for (uint32_t k = 0; k < 9 && same; k++) same = char(R8(text + 6 + at + k)) == want[k];
        if (same) return true;
    }
    return false;
}

bool PcEntryUp() { return g_pc.screen && g_pc.widget && Frame() - g_pc.frame <= 2; }

void OpenPcOptions() {
    LOGR("PC Options opened from the game's menus");
#ifdef _WIN32
    app::OpenPcOptions();
#endif
}

// each poll of a manager whose active screen is the Options menu: the copy follows SOUND's place and look
void UpdatePcEntry(uint32_t menu) {
    int16_t count = R16s(menu + 0x26);
    uint32_t items = R32(menu + 4);
    if (count < 2 || !items) return;
    uint32_t last = R32(items + uint32_t(count - 1) * 4), above = R32(items + uint32_t(count - 2) * 4);
    uint32_t anchor = last ? R32(last + 0x10) : 0, prev = above ? R32(above + 0x10) : 0;
    if (!anchor || !prev || R32(anchor) != kTextWidgetVtable) return;
    static uint32_t text = 0, second = 0;
    if (!text) text = GameString("PC OPTIONS");
    if (!g_pc.widget) g_pc.widget = mem::AllocSystem(kWidgetBytes);
    if (!text || !g_pc.widget) return;
    if (menu != g_pc.screen || anchor != g_pc.anchor) {
        g_pc.screen = menu;
        g_pc.anchor = anchor;
        g_pc.selected = false;
        // the second string the widget holds is shared: one more reference keeps it for our copy
        second = R32(anchor + 0x54);
        if (second) kernel::G<uint16_t>(second + 4) = uint16_t(kernel::G<uint16_t>(second + 4) + 1);
    }
    for (uint32_t k = 0; k < kWidgetBytes; k += 4) W32(g_pc.widget + k, R32(anchor + k));
    W32(g_pc.widget + 0x14, 0);
    W32(g_pc.widget + 0x1C, text);
    W32(g_pc.widget + 0x54, second);
    kernel::G<float>(g_pc.widget + 0x30) = 2 * RF(anchor + 0x30) - RF(prev + 0x30);
    uint32_t color = R32(anchor + 0x50) & 0xFFFFFF;
    W32(g_pc.widget + 0x50, color | (g_pc.selected ? kSelectedAlpha : kIdleAlpha));
    g_pc.frame = Frame();
}

// The pause menu: its Xbox Live Options line becomes PC Options (opening the PC settings page instead), and Options
// reads Game & Controller Options. The lines keep their place and the game's own selection; our strings are given
// a large reference count each frame so the game never frees them.
struct PauseEntry {
    uint32_t screen = 0;
    int16_t index = -1;  // the PC Options line
    uint32_t frame = 0;  // the last frame the pause menu was up
} g_pause;

void PauseMenu(uint32_t menu) {
    static uint32_t pc = 0, game = 0;
    if (!pc) pc = GameString("PC Options");
    if (!game) game = GameString("Game & Controller Options");
    uint32_t items = R32(menu + 4);
    int16_t count = R16s(menu + 0x26);
    if (!pc || !game || !items) return;
    for (int16_t i = 0; i < count; i++) {
        uint32_t item = R32(items + uint32_t(i) * 4), widget = item ? R32(item + 0x10) : 0;
        uint32_t text = widget && R32(widget) == kTextWidgetVtable ? R32(widget + 0x1C) : 0;
        if (!text) continue;
        if (text == pc || SameText(text, "Xbox Live Options")) {
            if (text != pc) W32(widget + 0x1C, pc);
            kernel::G<uint8_t>(item + 0x16) = 0;
            g_pause = {menu, i, Frame()};
        } else if (text != game && SameText(text, "Options")) {
            W32(widget + 0x1C, game);
        }
    }
    kernel::G<uint16_t>(pc + 4) = 0x4000;
    kernel::G<uint16_t>(game + 4) = 0x4000;
}

bool PausePcSelected() {
    return g_pause.screen && g_pause.index >= 0 && Frame() - g_pause.frame <= 2 &&
           R16s(g_pause.screen + 0x22) == g_pause.index && !R8(g_pause.screen + 0x30);
}

void PlayMoveSound(X86Context& ctx, uint32_t menu) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = menu;
    kernel::CallGuest(ctx, R32(R32(menu) + kSoundSlot), {});
    ctx.ecx = ecx;
}

void MenuPointer(X86Context& ctx, uint32_t manager) {
    uint32_t screens = R32(manager + 4);
    int32_t active = int32_t(R32(manager + 0xC));
    uint32_t menu = screens && active >= 0 ? R32(screens + uint32_t(active) * 4) : 0;
    uint32_t frame = Frame();
    {
        static uint32_t shown[2] = {};
        int m = manager == R32(kFrontEnd) ? 0 : manager == R32(kInGame) ? 1 : -1;
        uint32_t vt = menu ? R32(menu) : 0;
        if (m >= 0 && vt != shown[m]) LOGI("menu screen %d: %08x", m, shown[m] = vt);  // for the harness's scripts
    }
    if (manager == R32(kFrontEnd) || manager == R32(kInGame)) Typewriter(ctx, menu);
    if (menu && manager == R32(kFrontEnd) && R32(menu) == kMainMenuVtable) HideItem(ctx, menu, "Multiplayer");
    if (menu && manager == R32(kInGame)) PauseMenu(menu);
    bool options = menu && R32(menu) == kOptionsVtable && (manager == R32(kFrontEnd) || manager == R32(kInGame));
    if (options) {
        UpdatePcEntry(menu);
        if (g_pc.sound && menu == g_pc.screen) PlayMoveSound(ctx, menu);
        if (g_pc.select_first && menu == g_pc.screen) Select(ctx, menu, 0);
        g_pc.sound = g_pc.select_first = false;
    }
    std::vector<ItemBox> items = menu ? ShownItems(ctx, menu) : std::vector<ItemBox>{};
    if (manager == R32(kDialogs)) {
        if (items.empty()) return;
        g_dialog_frame = frame;
        g_dialog_shown = true;
    } else if (g_dialog_shown && frame - g_dialog_frame <= 2) {
        return;  // a dialog over this screen has the mouse
    } else if (items.empty() && manager != R32(kFrontEnd) &&
               !(menu && R32(menu) == kPressStartVtable && R8(menu + 0x38))) {
        return;  // in a level without a menu
    }
    g_pointer_frame = frame;
    g_cursor = true;
    input::SetMenuInput(true);
    Pointer pointer = TakePointer();
#ifdef _WIN32
    if (app::MenuOpen()) return;  // the settings panel has the mouse
#endif
    if (!pointer.inside) return;
    // between screens the front end has none (a level loading): nothing to point at. Reading the missing screen's
    // fields here crashed the game on a click while the first mission loaded.
    if (!menu) return;
    float x = pointer.x, y = pointer.y;
    // a click that could not select its item yet (the screen was locked) waits a little
    static int held_clicks = 0;
    static uint32_t held_since = 0;
    bool was_held = held_clicks && frame - held_since <= 30;
    if (was_held) pointer.left_clicks += held_clicks;
    held_clicks = 0;
    // text boxes of close lines overlap: the nearest line's
    const ItemBox* hit = nullptr;
    auto nearer = [&](const ItemBox& b) {
        auto mid = [](const ItemBox& i) { return (i.box.top + i.box.bottom) / 2; };
        return !hit || std::abs(y - mid(b)) < std::abs(y - mid(*hit));
    };
    for (const ItemBox& b : items)
        if (b.box.Contains(x, y) && nearer(b)) hit = &b;
    // right of a label on its line: the row's value
    float value_x = -1;
    if (!hit) {
        for (const ItemBox& b : items)
            if (x >= b.box.right && y >= b.box.top && y < b.box.bottom && nearer(b)) hit = &b;
        // only a row: an item with left/right neighbours is a grid's (the typewriter's keys)
        uint32_t row = hit ? R32(R32(menu + 4) + uint32_t(hit->index) * 4) : 0;
        if (row && (R16s(row + 0xC) >= 0 || R16s(row + 0xE) >= 0)) hit = nullptr;
        if (hit && (value_x = ValueMiddle(ctx, items, *hit)) < 0) hit = nullptr;
    }
    // PC OPTIONS under the Options menu's items
    if (options && menu == g_pc.screen && PcEntryUp()) {
        game::menu::Rect box;
        if (WidgetBox(ctx, g_pc.widget, box) && box.Contains(x, y)) {
            if ((pointer.moved || pointer.left_clicks) && !g_pc.selected && !R8(menu + 0x30)) {
                g_pc.selected = true;
                PlayMoveSound(ctx, menu);
            }
            if (pointer.left_clicks && g_pc.selected) OpenPcOptions();
            else if (pointer.right_clicks) TapB();
            return;
        }
        if (hit && pointer.moved) g_pc.selected = false;
    }
    bool on_item = hit != nullptr;
    if (pointer.left_clicks || pointer.right_clicks)
        LOGI("menu click at %.0f,%.0f: screen %08x, %d items, picker %d: item %d of %zu shown, value %.0f, selected %d, locked %d", x, y, menu ? R32(menu) : 0, menu ? R16s(menu + 0x26) : 0, menu ? int(Horizontal(menu)) : 0,
             hit ? hit->index : -1, items.size(), value_x, R16s(menu + 0x22), R8(menu + 0x30));
    int16_t selected = R16s(menu + 0x22);
    // a picker's choices (difficulty: Easy, Normal, Hard...) that are lines stacked in one place, showing one at a time:
    // the pointer over them picks none, the arrows and the wheel step, a click takes the one shown
    auto stacked = [&]() {
        int same = 0;
        for (const ItemBox& b : items)
            if (std::abs((b.box.top + b.box.bottom) / 2 - (hit->box.top + hit->box.bottom) / 2) < 4 &&
                std::abs((b.box.left + b.box.right) / 2 - (hit->box.left + hit->box.right) / 2) < 4)
                same++;
        return same > 1;
    };
    if (on_item && Horizontal(menu) && stacked()) {
        hit = nullptr;
        on_item = false;
        if (pointer.left_clicks) {
            TapA();
            return;
        }
    }
    // a hover the screen could not take while its highlight moved (locked) is taken once it can: a fast sweep over
    // the typewriter's keys ends on the key under the pointer
    static bool hover_pending = false;
    if (pointer.moved) hover_pending = true;
    if (!on_item) hover_pending = false;
    if (on_item && hit->index == selected) hover_pending = false;
    if (on_item && !R8(menu + 0x30) && (hover_pending || pointer.left_clicks) && hit->index != selected) {
        Select(ctx, menu, hit->index);
        hover_pending = false;
    }
    if (pointer.left_clicks && on_item && hit->index != R16s(menu + 0x22)) {
        if (!was_held) held_since = frame;
        held_clicks = pointer.left_clicks;
        return;
    }
    if (pointer.left_clicks && value_x >= 0) {
        constexpr float kValueMiddle = 10.0f;
        TapDpad(x < value_x - kValueMiddle ? kDpadLeft : kDpadRight);
        return;
    }
    // Bonus Materials (screen 0x82A708): the page on the right shows a movie or picture with an arrow each side of it
    // (about 291 and 589 across, 230 down in 640x480), stepped by left and right: a click on that page left or right of
    // its middle steps it
    if (pointer.left_clicks && !on_item && R32(menu) == kBonusVtable && x > 250.0f && y > 120.0f && y < 340.0f) {
        TapDpad(x < 440.0f ? kDpadLeft : kDpadRight);
        return;
    }
    bool no_items = !menu || R16s(menu + 0x26) <= 0;
    bool picker = menu && Horizontal(menu);
    constexpr float kPickerMiddle = 64.0f;
    // a picker's arrows are pictures, beside its middle (difficulty, chapter select): left or right of it steps
    if (pointer.left_clicks && picker && !on_item && std::abs(x - 320.0f) > kPickerMiddle)
        TapDpad(x < 320.0f ? kDpadLeft : kDpadRight);
    else if (pointer.left_clicks && (on_item || no_items || (picker && items.empty())))
        TapA();
    else if (pointer.right_clicks)
        TapB();
    else if (pointer.wheel && picker)
        TapDpad(pointer.wheel > 0 ? kDpadLeft : kDpadRight);
    else if (pointer.wheel)
        TapDpad(pointer.wheel > 0 ? kDpadUp : kDpadDown);
}

}  // namespace

namespace game {

bool MenuMouseCursorVisible() { return g_cursor && Frame() - g_pointer_frame <= 4; }
bool MenuHasPointer() { return Frame() - g_pointer_frame <= 2; }

// the pad as the game is about to see it: on the Options menu, down from its last item selects PC OPTIONS, and while
// that is selected up, down and A are its own; a direction or A it took stays hidden from the game until released
void OptionsPad(input::Pad& pad) {
    constexpr uint16_t kUp = 1, kDown = 2;
    constexpr int16_t kStick = 16000;
    constexpr uint8_t kPressed = 0x20;
    static bool was_up = false, was_down = false, was_a = false, was_b = false;
    static uint8_t hidden = 0;  // 1 up, 2 down, 4 A: taken, hidden until released
    // Y signs in to Xbox Live where its corner would be up; the profile screens keep theirs
    if (Frame() - g_live_frame <= 2) {
        uint32_t fe = R32(kFrontEnd), screens = fe ? R32(fe + 4) : 0;
        int32_t active = fe ? int32_t(R32(fe + 0xC)) : -1;
        uint32_t screen = screens && active >= 0 ? R32(screens + uint32_t(active) * 4) : 0;
        uint32_t vt = screen ? R32(screen) : 0;
        if (vt != kTypewriterVtable && vt != kProfilesVtable) pad.analog[3] = 0;
    }
    bool up = (pad.buttons & kUp) || pad.ly > kStick, down = (pad.buttons & kDown) || pad.ly < -kStick;
    bool a = pad.analog[0] > kPressed, b = pad.analog[1] > kPressed;
    bool up_edge = up && !was_up, down_edge = down && !was_down, a_edge = a && !was_a, b_edge = b && !was_b;
    was_up = up, was_down = down, was_a = a, was_b = b;
    // the pause menu's PC Options line: A opens the PC settings page, the game never sees it
    if (PausePcSelected() && a_edge) {
        hidden |= 4;
        OpenPcOptions();
    }
    if (PcEntryUp()) {
        uint32_t menu = g_pc.screen;
        bool last = R16s(menu + 0x22) == R16s(menu + 0x26) - 1, locked = R8(menu + 0x30);
        if (!g_pc.selected) {
            if (down_edge && last && !locked) {
                g_pc.selected = g_pc.sound = true;
                hidden |= 2;
            }
        } else if (up_edge) {
            g_pc.selected = false;
            g_pc.sound = true;
            hidden |= 1;
        } else if (down_edge) {
            g_pc.selected = false;
            g_pc.select_first = true;
            hidden |= 2;
        } else if (a_edge) {
            hidden |= 4;
            OpenPcOptions();
        } else if (b_edge) {
            g_pc.selected = false;
        }
        if (g_pc.selected) hidden |= (up ? 1 : 0) | (down ? 2 : 0) | (a ? 4 : 0);
    } else {
        g_pc.selected = false;
    }
    if (!up) hidden &= uint8_t(~1);
    if (!down) hidden &= uint8_t(~2);
    if (!a) hidden &= uint8_t(~4);
    if (hidden & 1) {
        pad.buttons &= uint16_t(~kUp);
        if (pad.ly > 0) pad.ly = 0;
    }
    if (hidden & 2) {
        pad.buttons &= uint16_t(~kDown);
        if (pad.ly < 0) pad.ly = 0;
    }
    if (hidden & 4) pad.analog[0] = 0;
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_0031FC10);
namespace {

uint32_t ClickToStart() {
    static uint32_t str = GameString("Click anywhere to start");
    return str;
}

bool StartPrompt() {
    uint32_t mode = game::Opt().button_prompts;
    return mode == 1 || (mode != 2 && input::KeyboardActive());
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00321160);
X86_EXTERN_FUNC(__imp__sub_00325980);

// a UI manager's pad poll (ecx manager)
X86_FUNC(sub_0031FC10) {
    {
        X86HostFp host_fp;
        MenuPointer(ctx, ctx.ecx);
    }
    __imp__sub_0031FC10(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_0030F900);
X86_EXTERN_FUNC(__imp__sub_00328E90);

// the main menu's tick (edi screen, the selected line on the stack): shows "check0<n+1>", the box printed beside line
// n. The lines below the hidden Multiplayer moved up one, so their tick goes one box up with them
X86_FUNC(sub_00328E90) {
    int32_t line = int32_t(R32(ctx.esp + 4));
    if (R32(ctx.edi) == kMainMenuVtable && g_main_gone >= 0 && line > g_main_gone) W32(ctx.esp + 4, uint32_t(line - 1));
    __imp__sub_00328E90(ctx, base);
}

// a button prompt's draw (ecx prompt, vtable 0x82A450: a button picture and words, as "Press A to select"). The
// front end's top right one is Y and "Xbox Live" (made at 0x32A660, at 603,52 in 640x480): not drawn
X86_FUNC(sub_0030F900) {
    uint32_t prompt = ctx.ecx;
    if (R32(prompt) == 0x82A450 && RF(prompt + 0x2C) > 560.0f && RF(prompt + 0x30) < 80.0f) {
        g_live_frame = Frame();
        ctx.esp += 4;  // the draw's own ret, skipped with it
        return;
    }
    __imp__sub_0030F900(ctx, base);
}

// a text widget's draw (ecx widget)
X86_FUNC(sub_00321160) {
    uint32_t widget = ctx.ecx;
    if (R8(widget + 0x10) & 4) {
        uint32_t frame = Frame();
        std::lock_guard lock(g_drawn_lock);
        if (g_drawn.size() > 1024) std::erase_if(g_drawn, [frame](const auto& e) { return frame - e.second > 2; });
        g_drawn[widget] = frame;
    }
    // Multiplayer and the Xbox Live corner are not drawn
    uint32_t drawn_text = R32(widget + 0x1C);
    uint32_t button = R32(widget + 0x54);
    bool live_corner = button && SameText(button, "ybutton") && RF(widget + 0x30) < 80;
    if (live_corner || Hidden(widget) || (drawn_text && XboxLiveText(drawn_text))) {
        ctx.esp += 4;  // the draw's own ret, skipped with it
        return;
    }
    // the title screen's "Press START" says what the mouse does while the keyboard and mouse are in use
    uint32_t text = R32(widget + 0x1C);
    uint32_t shown = text && StartPrompt() && SameText(text, "Press START") ? ClickToStart() : 0;
    if (shown) W32(widget + 0x1C, shown);
    // SOUND looks unselected while PC OPTIONS is, and PC OPTIONS is drawn right after it
    bool anchor = widget == g_pc.anchor && PcEntryUp();
    uint32_t color = R32(widget + 0x50);
    if (anchor && g_pc.selected) W32(widget + 0x50, (color & 0xFFFFFF) | kIdleAlpha);
    __imp__sub_00321160(ctx, base);
    if (anchor && g_pc.selected) W32(widget + 0x50, color);
    if (shown) W32(widget + 0x1C, text);
    if (anchor) {
        ctx.ecx = g_pc.widget;
        kernel::CallGuest(ctx, 0x321160, {});
        ctx.ecx = widget;
    }
}

// the typewriter types key eax: the layout's character for that key
X86_FUNC(sub_00325980) {
    int key = int(ctx.eax);
    if (key >= 0 && key < kKeys && Qwerty()) ctx.eax = uint32_t(GameIndex(KeyChar(key)));
    __imp__sub_00325980(ctx, base);
}
