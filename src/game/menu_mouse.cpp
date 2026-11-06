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
#include <cstdlib>
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
constexpr uint32_t kChapterVtable = 0x8299F0;     // Chapter Select: a picker, the chapter's picture in the middle
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
    bool select_last = false;   // back from PC Options: the game's selection on SOUND, ours on PC OPTIONS
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

// The main menu's lines, centred each on its own, start at one place beside their boxes: the left end of the line
// that starts furthest left, as the game first laid them out (kept for the screen; the selected line is drawn larger)
void AlignMainMenu(X86Context& ctx, uint32_t menu) {
    static uint32_t laid_out = 0;
    static float left = 0;
    uint32_t items = R32(menu + 4);
    int16_t count = R16s(menu + 0x26);
    if (menu != laid_out) {
        float most = INFINITY;
        for (int16_t i = 0; items && i < count; i++) {
            uint32_t item = R32(items + uint32_t(i) * 4), widget = item ? R32(item + 0x10) : 0;
            game::menu::Rect box;
            if (!widget || R32(widget) != kTextWidgetVtable || R8(item + 0x16) || (R8(widget + 0x5C) & 0x30)) continue;
            if (WidgetBox(ctx, widget, box)) most = std::min(most, box.left + 3.0f);  // (WidgetBox's margin)
        }
        if (!std::isfinite(most)) return;
        left = most;
        laid_out = menu;
        LOGI("main menu: lines start at %.1f", left);
    }
    for (int16_t i = 0; items && i < count; i++) {
        uint32_t item = R32(items + uint32_t(i) * 4), widget = item ? R32(item + 0x10) : 0;
        if (!widget || R32(widget) != kTextWidgetVtable) continue;
        kernel::G<uint8_t>(widget + 0x5C) = uint8_t((R8(widget + 0x5C) & ~0x30) | 0x10);  // its left end at x
        kernel::G<float>(widget + 0x2C) = left;
    }
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

// a text widget that is a line of the Options menu PC OPTIONS is under
bool PcEntryLine(uint32_t widget) {
    uint32_t menu = g_pc.screen, items = menu ? R32(menu + 4) : 0;
    int16_t count = menu ? R16s(menu + 0x26) : 0;
    for (int16_t i = 0; items && i < count; i++) {
        uint32_t item = R32(items + uint32_t(i) * 4);
        if (item && R32(item + 0x10) == widget) return true;
    }
    return false;
}

bool RequestPcScreen();  // below
bool PcScreenPending();   // asked for, not up yet

void OpenPcOptions() {
    // the front end's Options menu: the game's own Gameplay screen, made PC Options (PcScreenPoll). PC OPTIONS stays
    // selected until that is up, so no other line lights up on the way
    if (RequestPcScreen()) return;
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
        g_pc.selected = g_pc.select_last;
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

// (locked or not: a click right after the hover that selected it finds the screen still moving its highlight, and
// its A is ours all the same; PC Options then opens once the screen is still)
bool PausePcSelected() {
    return g_pause.screen && g_pause.index >= 0 && Frame() - g_pause.frame <= 2 &&
           R16s(g_pause.screen + 0x22) == g_pause.index;
}

// The button prompts along the foot of a screen ("Press A to select and B to go back", "A Select  B Profile
// Manager"): one text each (vtable 0x82A450, drawn by 0x30F900) with the buttons as icon codes 33-49 in it. A click on
// one presses the button whose part of the line it lands in: a button owns the words around it, halfway to the next.
std::unordered_map<uint32_t, uint32_t> g_prompts;  // prompt -> engine frame last drawn, under g_drawn_lock

void PressIcon(int code) {
    switch (code - 33) {
    case 0: input::MenuTap(0, 0x80); break;  // right trigger
    case 1: TapA(); break;
    case 2: input::MenuTap(0, 4); break;  // X
    case 3: input::MenuTap(0, 8); break;  // Y
    case 4: TapB(); break;
    case 5: input::MenuTap(0, 0x40); break;  // left trigger
    case 8: TapDpad(kDpadUp); break;
    case 9: TapDpad(kDpadRight); break;
    case 10: TapDpad(kDpadLeft); break;
    case 11: TapDpad(kDpadDown); break;
    case 12: input::MenuTap(0x10); break;  // start
    case 16: input::MenuTap(0x20); break;  // back
    default: break;
    }
}

bool ClickPrompt(X86Context& ctx, float x, float y) {
    std::vector<uint32_t> prompts;
    {
        std::lock_guard lock(g_drawn_lock);
        uint32_t frame = Frame();
        for (const auto& [p, f] : g_prompts)
            if (frame - f <= 2) prompts.push_back(p);
    }
    for (uint32_t prompt : prompts) {
        game::menu::Rect box;
        uint32_t text = R32(prompt + 0x1C);
        if (!text || !WidgetBox(ctx, prompt, box) || !box.Contains(x, y)) continue;
        // the line's characters with a rough width each (an icon is wider), and where its buttons are
        uint32_t len = kernel::G<uint16_t>(text + 2);
        if (len == 0 || len > 256) continue;
        std::vector<float> start;
        std::vector<std::pair<float, int>> icons;  // middle, code
        float at = 0;
        for (uint32_t i = 0; i < len; i++) {
            uint8_t c = R8(text + 6 + i);
            float w = (c >= 33 && c <= 49) ? 2.2f : c == ' ' ? 0.5f : 1.0f;
            if (c >= 33 && c <= 49) icons.push_back({at + w / 2, c});
            at += w;
        }
        if (icons.empty() || at <= 0) continue;
        float pos = (x - box.left) / (box.right - box.left) * at;
        size_t pick = 0;
        for (size_t k = 1; k < icons.size(); k++)
            if (pos > (icons[k - 1].first + icons[k].first) / 2) pick = k;
        LOGI("prompt click: button code %d", icons[pick].second);
        PressIcon(icons[pick].second);
        return true;
    }
    return false;
}

// ---- PC Options as one of the game's screens ---------------------------------------------------------------------
// PC OPTIONS in the front end's Options menu opens the Gameplay screen (vtable 0x828930, gameplay_menu.panel: its
// lines are items 0-4 with their values in the text widgets at +0x3C..+0x4C and arrows beside them, item 5 Reset to
// Defaults, 6 Apply and Exit), and PC Options in the pause menu the in-game Options screen (vtable 0x82A388: lines
// items 0-9, seven with a value text at +0x38..+0x50 and an arrow pair, three (the sensitivities and Volume) with a
// slider picture, 10 Reset Defaults, 11 Apply). While one is up for us its title, lines and values are the PC
// settings (app::pc, menu.cpp), a page at a time, the list scrolling at its ends. The game draws, highlights and
// moves the selection as on its own screens; left, right, A and X on our lines are ours and the game never sees them.
// On the in-game screen the slider lines are put aside (disabled, moved off) and the seven others closed up; their
// sliders are pictures of the screen's decorations (+0x34, drawn by 0x310010), two per slider, found by their place.
constexpr int kMostLines = 7;
constexpr int kMostArrows = 14;

struct Layout {
    uint32_t vtable;
    uint32_t manager;         // kFrontEnd or kInGame
    uint32_t pick_vtable;     // the screen it is picked from (the Options menu, the pause menu)
    int pick_item;            // the line picked there (GAMEPLAY, Options)
    int lines;                // the lines we use
    int items[kMostLines];    // their label items
    int aside[3];             // label items put aside, -1 none
    uint32_t values;          // the value texts, one per line, in order
    uint32_t arrows;          // the arrow pictures (vtable 0x82A2C0): a pair per line in order
    int arrow_count;
    int reset_item, apply_item;
    const char* title;        // the title text, lowercase, and ours
    const char* pc_title_caps;
    const char* pc_title;
    bool close_up;            // the lines take the places of the first ones (the slider lines' gaps closed)
    float most_width;         // a value wider is drawn smaller
    float key_offset;         // a binding's two keys: each this far from the value's middle, and this wide at most
    float key_width;
};

constexpr Layout kLayouts[] = {
    {0x828930, kFrontEnd, kOptionsVtable, 0, 5, {0, 1, 2, 3, 4, -1, -1}, {-1, -1, -1}, 0x3C, 0x50, 10, 5, 6,
     "gameplay", "PC OPTIONS", "PC Options", false, 140.0f, 44.0f, 84.0f},
    {0x82A388, kInGame, 0x8289F8, 1, 7, {0, 1, 4, 5, 7, 8, 9}, {2, 3, 6}, 0x38, 0x54, 14, 10, 11,
     "options", "PC OPTIONS", "PC Options", true, 150.0f, 48.0f, 90.0f},
};

struct PcScreen {
    const Layout* want_layout = nullptr;  // asked for: its line is picked on the screen it opens from, then it is ours
    bool tapped = false;
    uint32_t tapped_at = 0;
    uint32_t asked = 0;    // the poll it was asked on
    const Layout* layout = nullptr;
    uint32_t screen = 0;   // the host screen while it is ours
    uint32_t frame = 0;    // last poll with it up
    int top = 0;           // the first line shown
    bool applied = false;  // left by Apply
    uint32_t own_labels[kMostLines] = {}, own_values[kMostLines] = {};
    float own_label_y[kMostLines] = {}, own_value_y[kMostLines] = {};
    float own_arrows[kMostArrows][2] = {};
    float own_scales[kMostLines][2] = {};  // the values' letter size, x and y (+0x38, +0x3C)
    float own_aside_y[3] = {};
    uint8_t own_aside_disabled[3] = {};
    uint32_t idle_color = 0, lit_color = 0;  // a value's colour on a line not selected, and on the selected one
    // a binding line: its two keys (strings), the key the selection is on (-1 none) and whether it waits for a key
    bool row_binding[kMostLines] = {}, row_waiting[kMostLines] = {};
    int row_slot[kMostLines] = {};
    uint32_t row_keys[kMostLines][2] = {};
    std::unordered_map<uint32_t, uint32_t> titles;  // title widgets retitled, and the string each had
    std::unordered_map<uint32_t, uint32_t> want;  // text widget -> the string it shows, under g_drawn_lock
} g_pcs;

uint32_t PcString(const std::string& text) {
    static std::unordered_map<std::string, uint32_t> strings;
    auto it = strings.find(text);
    uint32_t s = it != strings.end() ? it->second : (strings[text] = GameString(text.c_str()));
    if (s) kernel::G<uint16_t>(s + 4) = 0x4000;  // never freed by the game
    return s;
}

uint32_t ActiveScreen(uint32_t manager_slot) {
    uint32_t m = R32(manager_slot), screens = m ? R32(m + 4) : 0;
    int32_t active = m ? int32_t(R32(m + 0xC)) : -1;
    return screens && active >= 0 ? R32(screens + uint32_t(active) * 4) : 0;
}

uint32_t LineItem(uint32_t menu, int item) {
    uint32_t items = R32(menu + 4);
    return items && item >= 0 && item < R16s(menu + 0x26) ? R32(items + uint32_t(item) * 4) : 0;
}

// PC OPTIONS picked on the front end's Options menu, or PC Options on the pause menu
bool RequestPcScreen() {
    for (const Layout& l : kLayouts) {
        uint32_t screen = ActiveScreen(l.manager);
        if (!screen || R32(screen) != l.pick_vtable) continue;
        if (l.manager == kFrontEnd && screen != g_pc.screen) continue;
        g_pcs.want_layout = &l;
        g_pcs.tapped = false;
        g_pcs.asked = 0;  // counted in polls from the next one
        return true;
    }
    return false;
}

// a picture's middle to x, y: its pictures (list +0x20, count +0x28) keep their corners in 640x480 at +0x40 (x, y),
// 0x14 apart
void MovePicture(uint32_t arrow, float x, float y) {
    kernel::G<float>(arrow + 0x14) = x;
    kernel::G<float>(arrow + 0x18) = y;
    uint32_t pictures = R32(arrow + 0x20), count = R32(arrow + 0x28);
    for (uint32_t p = 0; pictures && p < count && p < 4; p++) {
        uint32_t corners = R32(pictures + p * 4) + 0x40;
        if (corners == 0x40) continue;
        for (int axis = 0; axis < 2; axis++) {
            float lo = INFINITY, hi = -INFINITY;
            for (uint32_t c = 0; c < 4; c++) {
                lo = std::min(lo, RF(corners + c * 0x14 + uint32_t(axis) * 4));
                hi = std::max(hi, RF(corners + c * 0x14 + uint32_t(axis) * 4));
            }
            float shift = (axis ? y : x) - (lo + hi) / 2;
            if (!std::isfinite(shift) || std::fabs(shift) < 0.01f) continue;
            for (uint32_t c = 0; c < 4; c++) kernel::G<float>(corners + c * 0x14 + uint32_t(axis) * 4) += shift;
        }
    }
}

bool PcScreenUp() { return g_pcs.screen && Frame() - g_pcs.frame <= 2; }

uint32_t PcArrow(int a) { return a < g_pcs.layout->arrow_count ? R32(g_pcs.screen + g_pcs.layout->arrows + uint32_t(a) * 4) : 0; }

void LeavePcScreen() {
    const Layout& l = *g_pcs.layout;
    uint32_t menu = g_pcs.screen;
    for (int i = 0; i < l.lines; i++) {
        uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
        if (label && g_pcs.own_labels[i]) {
            W32(label + 0x1C, g_pcs.own_labels[i]);
            kernel::G<float>(label + 0x30) = g_pcs.own_label_y[i];
        }
        uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
        if (value && g_pcs.own_values[i]) {
            W32(value + 0x1C, g_pcs.own_values[i]);
            kernel::G<float>(value + 0x30) = g_pcs.own_value_y[i];
        }
        if (value && g_pcs.own_scales[i][0] > 0) {
            kernel::G<float>(value + 0x38) = g_pcs.own_scales[i][0];
            kernel::G<float>(value + 0x3C) = g_pcs.own_scales[i][1];
        }
    }
    for (int a = 0; a < l.arrow_count; a++) {
        uint32_t arrow = PcArrow(a);
        if (arrow) MovePicture(arrow, g_pcs.own_arrows[a][0], g_pcs.own_arrows[a][1]);
    }
    for (int k = 0; k < 3; k++) {
        uint32_t item = LineItem(menu, l.aside[k]), label = item ? R32(item + 0x10) : 0;
        if (!item) continue;
        kernel::G<uint8_t>(item + 0x16) = g_pcs.own_aside_disabled[k];
        if (label) kernel::G<float>(label + 0x30) = g_pcs.own_aside_y[k];
    }
    for (auto [title, text] : g_pcs.titles) W32(title + 0x1C, text);
    g_pcs.titles.clear();
    {
        std::lock_guard lock(g_drawn_lock);
        g_pcs.want.clear();
    }
    app::pc::Leave(g_pcs.applied);
    LOGR("PC Options closed%s", g_pcs.applied ? ", applied" : "");
    g_pcs.screen = 0;
    g_pcs.layout = nullptr;
}

// the host screen taken: what it had is kept to put back, the slider lines put aside, the lines closed up
void TakePcScreen(uint32_t menu, const Layout& l) {
    g_pcs.want_layout = nullptr;
    g_pcs.layout = &l;
    g_pcs.screen = menu;
    g_pcs.top = 0;
    g_pcs.applied = false;
    float first_y = 0, step_y = 0;
    int16_t selected = R16s(menu + 0x22);
    g_pcs.idle_color = 0;
    for (int i = 0; i < l.lines; i++) {
        uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_labels[i] = label ? R32(label + 0x1C) : 0;
        g_pcs.row_binding[i] = g_pcs.row_waiting[i] = false;
        g_pcs.row_slot[i] = -1;
        g_pcs.row_keys[i][0] = g_pcs.row_keys[i][1] = 0;
        g_pcs.own_label_y[i] = label ? RF(label + 0x30) : 0;
        uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
        g_pcs.own_values[i] = value ? R32(value + 0x1C) : 0;
        g_pcs.own_value_y[i] = value ? RF(value + 0x30) : 0;
        g_pcs.own_scales[i][0] = value ? RF(value + 0x38) : 0;
        g_pcs.own_scales[i][1] = value ? RF(value + 0x3C) : 0;
        if (value && l.items[i] != selected && !g_pcs.idle_color) g_pcs.idle_color = R32(value + 0x50);
        if (value && l.items[i] == selected) g_pcs.lit_color = R32(value + 0x50);
        if (i == 0) first_y = g_pcs.own_label_y[0];
        if (i == 1) step_y = (g_pcs.own_label_y[1] - first_y) / float(l.items[1] - l.items[0]);
    }
    for (int a = 0; a < l.arrow_count; a++) {
        uint32_t arrow = PcArrow(a);
        g_pcs.own_arrows[a][0] = arrow ? RF(arrow + 0x14) : 0;
        g_pcs.own_arrows[a][1] = arrow ? RF(arrow + 0x18) : 0;
    }
    for (int k = 0; k < 3; k++) {
        uint32_t item = LineItem(menu, l.aside[k]), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_aside_disabled[k] = item ? R8(item + 0x16) : 0;
        g_pcs.own_aside_y[k] = label ? RF(label + 0x30) : 0;
        if (!item) continue;
        kernel::G<uint8_t>(item + 0x16) = 1;
        if (label) kernel::G<float>(label + 0x30) = -200.0f;
    }
    if (l.close_up && step_y > 0) {
        for (int i = 0; i < l.lines; i++) {
            float y = first_y + step_y * float(i);
            uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
            uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
            if (label) kernel::G<float>(label + 0x30) = y;
            if (value) kernel::G<float>(value + 0x30) = y + (g_pcs.own_value_y[i] - g_pcs.own_label_y[i]);
            for (int side = 0; side < 2; side++) {
                int a = i * 2 + side;
                uint32_t arrow = PcArrow(a);
                if (arrow) MovePicture(arrow, g_pcs.own_arrows[a][0], y + (g_pcs.own_arrows[a][1] - g_pcs.own_label_y[i]));
            }
        }
    }
    if (l.manager == kFrontEnd) g_pc.selected = false;
    LOGR("PC Options opened on the game's %s screen", l.manager == kFrontEnd ? "Gameplay" : "Options");
}

bool PcScreenPending() { return g_pcs.want_layout != nullptr; }

// each poll of a manager: PC Options asked for, taken, kept up to date, and left
void PcScreenPoll(X86Context& ctx, uint32_t manager, uint32_t menu) {
    static uint32_t polls = 0;
    uint32_t frame = Frame(), poll = ++polls;
    const Layout* want = g_pcs.want_layout;
    if (want && manager == R32(want->manager)) {
        if (!g_pcs.asked) g_pcs.asked = poll;
        if (poll - g_pcs.asked > 300) {  // never came up
            g_pcs.want_layout = nullptr;
            g_pc.selected = false;
        }
        // the line picked once the A that asked is let go (a held A hides the tap), again if it did not take
        if (g_pcs.tapped && poll - g_pcs.tapped_at > 40) g_pcs.tapped = false;
        if (menu && R32(menu) == want->pick_vtable && !g_pcs.tapped && !R8(menu + 0x30) && poll - g_pcs.asked >= 6) {
            Select(ctx, menu, int16_t(want->pick_item));
            TapA();
            g_pcs.tapped = true;
            g_pcs.tapped_at = poll;
        } else if (menu && R32(menu) == want->vtable && g_pcs.tapped) {
            TakePcScreen(menu, *want);
        }
    }
    if (!g_pcs.screen || manager != R32(g_pcs.layout->manager)) return;
    if (menu != g_pcs.screen) {
        const Layout& left = *g_pcs.layout;
        LeavePcScreen();
        // back on the pause menu or the Options menu, the selection is on PC Options again
        if (left.manager == kInGame && menu && R32(menu) == left.pick_vtable && menu == g_pause.screen && g_pause.index >= 0)
            Select(ctx, menu, g_pause.index);
        if (left.manager == kFrontEnd) g_pc.select_last = true;
        return;
    }
    const Layout& l = *g_pcs.layout;
    g_pcs.frame = frame;
    int selected = R16s(menu + 0x22), row = -1;
    for (int i = 0; i < l.lines; i++)
        if (l.items[i] == selected) row = i;
    std::vector<app::pc::Line> lines = app::pc::Lines(row >= 0 ? g_pcs.top + row : -1);
    int rows = 0;
    while (rows < int(lines.size()) && !lines[size_t(rows)].action) rows++;
    g_pcs.top = std::clamp(g_pcs.top, 0, std::max(0, rows - l.lines));
    {
        std::lock_guard lock(g_drawn_lock);
        g_pcs.want.clear();
        for (int i = 0; i < l.lines; i++) {
            int line = g_pcs.top + i;
            uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
            uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
            const app::pc::Line* pl = line < rows ? &lines[size_t(line)] : nullptr;
            std::string lt = pl ? pl->label : "";
            std::string v = pl ? pl->value : "";
            if (label) W32(label + 0x1C, g_pcs.want[label] = PcString(lt));
            if (value) W32(value + 0x1C, g_pcs.want[value] = PcString(v));
            g_pcs.row_binding[i] = pl && pl->binding;
            g_pcs.row_waiting[i] = pl && pl->binding && pl->waiting;
            g_pcs.row_slot[i] = pl && pl->binding ? pl->slot : -1;
            for (int k = 0; k < 2; k++) g_pcs.row_keys[i][k] = pl && pl->binding ? PcString(pl->keys[k]) : 0;
        }
    }
    // the arrows beside a value move out to clear one longer than the game's own on and off, and a value too long for
    // the room (two key boxes) is drawn smaller
    for (int i = 0; i < l.lines; i++) {
        uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
        if (!value || !(g_pcs.own_scales[i][0] > 0) || g_pcs.row_binding[i]) continue;
        kernel::G<float>(value + 0x38) = g_pcs.own_scales[i][0];
        kernel::G<float>(value + 0x3C) = g_pcs.own_scales[i][1];
        float width = WidgetMethod(ctx, kWidgetWidth, value);
        if (!std::isfinite(width) || width < 0) width = 0;
        if (width > l.most_width) {
            float shrink = l.most_width / width;
            kernel::G<float>(value + 0x38) = g_pcs.own_scales[i][0] * shrink;
            kernel::G<float>(value + 0x3C) = g_pcs.own_scales[i][1] * shrink;
            width = l.most_width;
        }
        float middle = RF(value + 0x2C);
        constexpr float kGap = 14.0f;  // from the value's end to an arrow's middle
        for (int side = 0; side < 2; side++) {
            int a = i * 2 + side;
            uint32_t arrow = PcArrow(a);
            if (!arrow) continue;
            float own = g_pcs.own_arrows[a][0];
            MovePicture(arrow, own < middle ? std::min(own, middle - width / 2 - kGap)
                                            : std::max(own, middle + width / 2 + kGap), RF(arrow + 0x18));
        }
    }
}

// a picture about to be drawn: a slider of a line put aside (at its row, between the arrows) is not, nor a binding
// line's arrows; the other arrows are
bool PcScreenHidesPicture(uint32_t widget) {
    if (!PcScreenUp()) return false;
    const Layout& l = *g_pcs.layout;
    for (int a = 0; a < l.lines * 2; a++)
        if (PcArrow(a) == widget) return g_pcs.row_binding[a / 2];  // a binding line has keys to click, no arrows
    float x = RF(widget + 0x14), y = RF(widget + 0x18);
    for (int k = 0; k < 3; k++)
        if (l.aside[k] >= 0 && std::fabs(y - g_pcs.own_aside_y[k]) < 4.0f && x > 400.0f && x < 490.0f) return true;
    return false;
}

// a binding line's value about to be drawn (ecx the widget): its two keys instead, one each side of the value's
// middle, each shrunk to fit its room. On the selected line the key the selection is on keeps the line's colour and
// the other the colour of a line not selected; a key waiting to be pressed blinks. True when drawn here.
bool g_key_draw = false;  // a binding's key being drawn: its draw comes back through the text draw

bool PcScreenDrawBinding(X86Context& ctx, uint32_t widget) {
    if (g_key_draw || !PcScreenUp()) return false;
    const Layout& l = *g_pcs.layout;
    uint32_t menu = g_pcs.screen;
    int row = -1;
    for (int i = 0; i < l.lines; i++)
        if (R32(menu + l.values + uint32_t(i) * 4) == widget) row = i;
    if (row < 0 || !g_pcs.row_binding[row] || !(g_pcs.own_scales[row][0] > 0)) return false;
    bool on = R16s(menu + 0x22) == l.items[row];
    uint32_t text = R32(widget + 0x1C), color = R32(widget + 0x50);
    float x = RF(widget + 0x2C), y = RF(widget + 0x30);
    uint32_t idle = g_pcs.idle_color ? g_pcs.idle_color : color, lit = on || !g_pcs.lit_color ? color : g_pcs.lit_color;
    bool blink = (Frame() / 16) & 1;
    for (int k = 0; k < 2; k++) {
        uint32_t key = g_pcs.row_keys[row][k];
        if (!key) continue;
        kernel::G<uint16_t>(key + 4) = 0x4000;
        W32(widget + 0x1C, key);
        kernel::G<float>(widget + 0x38) = g_pcs.own_scales[row][0];
        kernel::G<float>(widget + 0x3C) = g_pcs.own_scales[row][1];
        float width = WidgetMethod(ctx, kWidgetWidth, widget), height = WidgetMethod(ctx, kWidgetHeight, widget);
        kernel::G<float>(widget + 0x30) = y;
        if (std::isfinite(width) && width > l.key_width) {  // smaller, and kept in the middle of the line
            float shrink = l.key_width / width;
            kernel::G<float>(widget + 0x38) = g_pcs.own_scales[row][0] * shrink;
            kernel::G<float>(widget + 0x3C) = g_pcs.own_scales[row][1] * shrink;
            if (std::isfinite(height) && height > 0) kernel::G<float>(widget + 0x30) = y + height * (1 - shrink) / 2;
        }
        kernel::G<float>(widget + 0x2C) = x + (k ? l.key_offset : -l.key_offset);
        bool waiting = g_pcs.row_waiting[row] && g_pcs.row_slot[row] == k;
        bool picked = on && g_pcs.row_slot[row] == k;
        W32(widget + 0x50, waiting ? (blink ? lit : idle) : picked ? color : on ? idle : color);
        ctx.ecx = widget;
        g_key_draw = true;
        kernel::CallGuest(ctx, 0x321160, {});
        g_key_draw = false;
    }
    W32(widget + 0x1C, text);
    W32(widget + 0x50, color);
    kernel::G<float>(widget + 0x2C) = x;
    kernel::G<float>(widget + 0x30) = y;
    kernel::G<float>(widget + 0x38) = g_pcs.own_scales[row][0];
    kernel::G<float>(widget + 0x3C) = g_pcs.own_scales[row][1];
    return true;
}

// a click right of a line's label (item, at x) while PC Options is up: on a binding line it is on one of the two keys:
// that key is picked and waits for its new key (a right click empties it). True when taken.
bool PcScreenClick(X86Context& ctx, uint32_t menu, int item, float x, bool right) {
    if (!PcScreenUp() || menu != g_pcs.screen) return false;
    const Layout& l = *g_pcs.layout;
    int row = -1;
    for (int i = 0; i < l.lines; i++)
        if (l.items[i] == item) row = i;
    if (row < 0 || !g_pcs.row_binding[row]) return false;
    uint32_t value = R32(menu + l.values + uint32_t(row) * 4);
    if (!value) return false;
    if (R16s(menu + 0x22) != item) {
        if (R8(menu + 0x30)) return true;  // locked: not yet
        Select(ctx, menu, int16_t(item));
    }
    int line = g_pcs.top + row;
    app::pc::Step(line, x < RF(value + 0x2C) ? -1 : 1);
    if (right) app::pc::Clear(line);
    else app::pc::Press(line);
    return true;
}

// a text widget about to be drawn: one of ours shows our words; the screen's title reads PC Options
void PcScreenText(uint32_t widget) {
    if (!PcScreenUp() || g_key_draw) return;
    const Layout& l = *g_pcs.layout;
    uint32_t want = 0;
    {
        std::lock_guard lock(g_drawn_lock);
        auto it = g_pcs.want.find(widget);
        if (it != g_pcs.want.end()) want = it->second;
    }
    uint32_t text = R32(widget + 0x1C);
    if (!want && text) {
        std::string t;
        for (uint32_t k = 0; k < 16; k++) {
            char c = char(R8(text + 6 + k));
            if (!c) break;
            t += char(std::tolower(uint8_t(c)));
        }
        // the title, above the lines (a menu's own line of the same name, drawn as it slides away, stays)
        if (t == l.title && RF(widget + 0x30) < 110.0f) {
            want = PcString(std::isupper(uint8_t(R8(text + 7))) ? l.pc_title_caps : l.pc_title);
            g_pcs.titles.emplace(widget, text);
        }
    }
    if (want) {
        kernel::G<uint16_t>(want + 4) = 0x4000;
        W32(widget + 0x1C, want);
    }
}

// the pad as the game is about to see it, while PC Options is up: our lines' left, right, A and X are ours, up and
// down at the list's ends scroll it, and a key box waiting for a key has every button. True when it is up.
bool PcScreenPad(input::Pad& pad) {
    static uint16_t was = 0, hidden = 0;
    static bool up = false;
    if (!PcScreenUp()) {
        was = hidden = 0;
        up = false;
        return false;
    }
    const Layout& l = *g_pcs.layout;
    constexpr uint16_t kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kA = 0x100, kB = 0x200, kX = 0x400;
    constexpr int16_t kStick = 16000;
    constexpr uint8_t kPressed = 0x20;
    app::pc::Update();
    uint16_t now = uint16_t(pad.buttons & 0xF);
    if (pad.ly > kStick) now |= kUp;
    if (pad.ly < -kStick) now |= kDown;
    if (pad.lx < -kStick) now |= kLeft;
    if (pad.lx > kStick) now |= kRight;
    if (pad.analog[0] > kPressed) now |= kA;
    if (pad.analog[1] > kPressed) now |= kB;
    if (pad.analog[2] > kPressed) now |= kX;
    if (!up) was = hidden = now;  // what picked PC Options, still held, is not a press on its first line
    up = true;
    uint16_t edge = uint16_t(now & ~was);
    was = now;
    uint32_t menu = g_pcs.screen;
    int selected = R16s(menu + 0x22), row = -1;
    for (int i = 0; i < l.lines; i++)
        if (l.items[i] == selected) row = i;
    bool locked = R8(menu + 0x30);
    if (app::pc::Capturing()) {
        hidden = now;  // the key box has them all, until each is let go
    } else if (!locked) {
        std::vector<app::pc::Line> lines = app::pc::Lines(-1);
        int rows = 0;
        while (rows < int(lines.size()) && !lines[size_t(rows)].action) rows++;
        int line = g_pcs.top + row;
        if ((edge & kUp) && row == 0 && g_pcs.top > 0) {
            g_pcs.top--;
            hidden |= kUp;
        } else if ((edge & kDown) && row == l.lines - 1 && g_pcs.top + l.lines < rows) {
            g_pcs.top++;
            hidden |= kDown;
        }
        if (row >= 0 && line < rows) {
            if (edge & (kLeft | kRight)) app::pc::Step(line, (edge & kLeft) ? -1 : 1);
            if (edge & kA) app::pc::Press(line);
            if (edge & kX) app::pc::Clear(line);
            hidden |= edge & (kLeft | kRight | kA | kX);
        } else if (selected == l.reset_item && (edge & kA)) {  // Reset to Defaults: ours
            for (int i = rows; i < int(lines.size()); i++)
                if (lines[size_t(i)].label == "Reset to Defaults") app::pc::Press(i);
            hidden |= kA;
        } else if (selected == l.apply_item && (edge & kA)) {  // Apply: the game leaves, we apply
            g_pcs.applied = true;
        }
        if (edge & kB) g_pcs.applied = false;
    }
    hidden &= now;
    if (hidden & (kUp | kDown | kLeft | kRight)) pad.buttons &= uint16_t(~(hidden & 0xF));
    if ((hidden & kUp) && pad.ly > 0) pad.ly = 0;
    if ((hidden & kDown) && pad.ly < 0) pad.ly = 0;
    if ((hidden & (kLeft | kRight)) && pad.lx) pad.lx = 0;
    if (hidden & kA) pad.analog[0] = 0;
    if (hidden & kB) pad.analog[1] = 0;
    if (hidden & kX) pad.analog[2] = 0;
    if (app::pc::Capturing()) {
        pad.buttons = 0;
        std::fill(std::begin(pad.analog), std::end(pad.analog), uint8_t(0));
        pad.lx = pad.ly = pad.rx = pad.ry = 0;
    }
    return true;
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
    if (menu && manager == R32(kFrontEnd) && R32(menu) == kMainMenuVtable) {
        HideItem(ctx, menu, "Multiplayer");
        AlignMainMenu(ctx, menu);
    }
    if (menu && manager == R32(kInGame)) PauseMenu(menu);
    if (manager == R32(kFrontEnd) || manager == R32(kInGame)) PcScreenPoll(ctx, manager, menu);
    if (manager == R32(kFrontEnd) && menu && R32(menu) != kOptionsVtable && R32(menu) != kLayouts[0].vtable)
        g_pc.select_last = false;
    bool options = menu && R32(menu) == kOptionsVtable && (manager == R32(kFrontEnd) || manager == R32(kInGame));
    if (options) {
        UpdatePcEntry(menu);
        if (g_pc.sound && menu == g_pc.screen) PlayMoveSound(ctx, menu);
        if (g_pc.select_first && menu == g_pc.screen) Select(ctx, menu, 0);
        if (g_pc.select_last && menu == g_pc.screen && !R8(menu + 0x30)) {
            Select(ctx, menu, int16_t(R16s(menu + 0x26) - 1));
            g_pc.selected = true;
            g_pc.select_last = false;
        }
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
    if (PcScreenUp() && app::pc::Capturing()) return;  // a key box waits for its key: the pointer moves nothing
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
    // PC Options: a binding line's keys
    if ((pointer.left_clicks || pointer.right_clicks) && hit && value_x >= 0 &&
        PcScreenClick(ctx, menu, hit->index, x, pointer.right_clicks > 0))
        return;
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
    // or lines that overlap (a choice not shown can still have a box where the shown one is): the same
    auto overlapped = [&]() {
        for (const ItemBox& b : items)
            if (b.index != hit->index && b.box.left < hit->box.right && hit->box.left < b.box.right &&
                b.box.top < hit->box.bottom && hit->box.top < b.box.bottom)
                return true;
        return false;
    };
    // a picker's choices never follow the pointer alone: a click picks
    bool picker_hover = on_item && Horizontal(menu);
    if (on_item && Horizontal(menu) && (stacked() || overlapped())) {
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
    if (picker_hover) hover_pending = false;
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
    // a button prompt at the foot of the screen
    if (pointer.left_clicks && !on_item && ClickPrompt(ctx, x, y)) return;
    // Bonus Materials (screen 0x82A708): the page on the right shows a movie or picture with an arrow each side of it
    // (about 291 and 589 across, 230 down in 640x480), stepped by left and right: a click on that page left or right of
    // its middle steps it
    if (pointer.left_clicks && !on_item && R32(menu) == kBonusVtable && x > 250.0f && y > 120.0f && y < 340.0f) {
        TapDpad(x < 440.0f ? kDpadLeft : kDpadRight);
        return;
    }
    // Chapter Select: the chapter's picture (about 186 to 456 across, 156 to 305 down in 640x480) starts it
    if (pointer.left_clicks && !on_item && R32(menu) == kChapterVtable && x > 186.0f && x < 456.0f && y > 156.0f &&
        y < 305.0f) {
        TapA();
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
    bool held_up = (pad.buttons & kUp) || pad.ly > kStick, held_down = (pad.buttons & kDown) || pad.ly < -kStick;
    bool held_a = pad.analog[0] > kPressed, held_b = pad.analog[1] > kPressed;
    if (PcScreenPad(pad)) {  // PC Options has the pad: what is held now is no press once it closes
        was_up = held_up, was_down = held_down, was_a = held_a, was_b = held_b;
        return;
    }
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
    if (PcEntryUp() && PcScreenPending()) {
        // what asked for it, still held, stays hidden until let go; the tap that picks the line goes through
    } else if (PcEntryUp()) {
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

uint32_t ClickToStartGame() {
    static uint32_t str = GameString("Click to start the game");
    return str;
}

// a game string holding the words
bool HasText(uint32_t str, const char* want) {
    uint32_t len = kernel::G<uint16_t>(str + 2), n = uint32_t(std::strlen(want));
    if (len < n || len > 256) return false;
    for (uint32_t at = 0; at + n <= len; at++) {
        bool same = true;
        for (uint32_t k = 0; k < n && same; k++) same = char(R8(str + 6 + at + k)) == want[k];
        if (same) return true;
    }
    return false;
}

bool StartPrompt() {
    uint32_t mode = game::Opt().button_prompts;
    return mode == 1 || (mode != 2 && input::KeyboardActive());
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00321160);
X86_EXTERN_FUNC(__imp__sub_00316DD0);

// a picture widget drawn (0x316DD0, vtable slot +0x44, ecx the widget): the sliders of the lines PC Options puts aside
// are not
X86_FUNC(sub_00316DD0) {
    if (PcScreenHidesPicture(ctx.ecx)) {
        ctx.esp += 4;  // its ret, skipped with it
        return;
    }
    __imp__sub_00316DD0(ctx, base);
}
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
    if (R32(prompt) == 0x82A450) {
        std::lock_guard lock(g_drawn_lock);
        if (g_prompts.size() > 64) g_prompts.clear();
        g_prompts[prompt] = Frame();
    }
    // a mission's "Press A to start the game" says what the mouse does while the keyboard and mouse are in use
    uint32_t text = R32(prompt + 0x1C);
    uint32_t shown = text && StartPrompt() && HasText(text, "to start the game") ? ClickToStartGame() : 0;
    if (shown) W32(prompt + 0x1C, shown);
    __imp__sub_0030F900(ctx, base);
    if (shown) W32(prompt + 0x1C, text);
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
    PcScreenText(widget);
    if (PcScreenDrawBinding(ctx, widget)) {
        ctx.esp += 4;  // the draw's own ret, skipped with it
        return;
    }
    // the title screen's "Press START" says what the mouse does while the keyboard and mouse are in use
    uint32_t text = R32(widget + 0x1C);
    uint32_t shown = text && StartPrompt() && SameText(text, "Press START") ? ClickToStart() : 0;
    if (shown) W32(widget + 0x1C, shown);
    // the Options menu's lines look unselected while PC OPTIONS is, and PC OPTIONS is drawn right after SOUND
    bool anchor = widget == g_pc.anchor && PcEntryUp();
    bool dimmed = PcEntryUp() && g_pc.selected && (anchor || PcEntryLine(widget));
    uint32_t color = R32(widget + 0x50);
    if (dimmed) W32(widget + 0x50, (color & 0xFFFFFF) | kIdleAlpha);
    __imp__sub_00321160(ctx, base);
    if (dimmed) W32(widget + 0x50, color);
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
