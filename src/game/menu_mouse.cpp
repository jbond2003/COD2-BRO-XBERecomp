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
#include <unordered_map>
#include <vector>

#include "app/app.h"
#include "common/menu_pointer.h"
#include "common/menu_state.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"

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

void MenuPointer(X86Context& ctx, uint32_t manager) {
    uint32_t screens = R32(manager + 4);
    int32_t active = int32_t(R32(manager + 0xC));
    uint32_t menu = screens && active >= 0 ? R32(screens + uint32_t(active) * 4) : 0;
    uint32_t frame = Frame();
    if (manager == R32(kFrontEnd) || manager == R32(kInGame)) Typewriter(ctx, menu);
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
    if (!pointer.inside) return;
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
    bool on_item = hit != nullptr;
    if (pointer.left_clicks || pointer.right_clicks)
        LOGI("menu click at %.0f,%.0f: item %d of %zu shown, value %.0f, selected %d, locked %d", x, y,
             hit ? hit->index : -1, items.size(), value_x, R16s(menu + 0x22), R8(menu + 0x30));
    int16_t selected = R16s(menu + 0x22);
    if (on_item && !R8(menu + 0x30) && (pointer.moved || pointer.left_clicks) && hit->index != selected)
        Select(ctx, menu, hit->index);
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
    bool no_items = !menu || R16s(menu + 0x26) <= 0;
    bool picker = menu && Horizontal(menu);
    constexpr float kPickerMiddle = 64.0f;
    if (pointer.left_clicks && picker && items.empty() && std::abs(x - 320.0f) > kPickerMiddle)
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

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_0031FC10);
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

// a text widget's draw (ecx widget)
X86_FUNC(sub_00321160) {
    uint32_t widget = ctx.ecx;
    if (R8(widget + 0x10) & 4) {
        uint32_t frame = Frame();
        std::lock_guard lock(g_drawn_lock);
        if (g_drawn.size() > 1024) std::erase_if(g_drawn, [frame](const auto& e) { return frame - e.second > 2; });
        g_drawn[widget] = frame;
    }
    __imp__sub_00321160(ctx, base);
}

// the typewriter types key eax: the layout's character for that key
X86_FUNC(sub_00325980) {
    int key = int(ctx.eax);
    if (key >= 0 && key < kKeys && Qwerty()) ctx.eax = uint32_t(GameIndex(KeyChar(key)));
    __imp__sub_00325980(ctx, base);
}
