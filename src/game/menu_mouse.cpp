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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
#include "gpu/backend.h"
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
constexpr uint32_t kChapterVtable = 0x8299F0;     // Chapter Select: a picker, the chapter's picture in the middle
constexpr uint32_t kTypewriterVtable = 0x82A068;
constexpr uint32_t kProfilesVtable = 0x828ED0;  // Profile Manager
constexpr uint32_t kBonusVtable = 0x82A708;     // Bonus Materials
// the Bonus Materials pages with a picture and an arrow each side of it: the hub, Concept Art, Vehicles, Weapons,
// Characters and the collector's edition art (each its own screen)
constexpr uint32_t kBonusPageVtables[] = {kBonusVtable, 0x828F98, 0x829AB8, 0x828868, 0x829728, 0x829ED8};
bool BonusPage(uint32_t menu) {
    for (uint32_t vt : kBonusPageVtables)
        if (R32(menu) == vt) return true;
    return false;
}
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
// The front end draws every screen's art in a band of the 640x480 frame, rows 65 to 412, black above and below, and
// the button prompts ("A Select  B Back") on the black under it, down to row 445. On a widescreen output the art's
// band alone is shown, over the whole window: no bars, and the screens close to their 4:3 shape (the whole frame
// over 16:9 made them a third too wide). What the game draws under the art is drawn kFootLift higher, on the art's
// last rows, with a black edge around its letters so they read on whatever is behind them.
constexpr float kArtBottom = 412.0f, kFootLift = 36.0f;
// The band fills the window (his choice, after trying bars): it is wider than a 16:9 or 16:10 window, so up to
// kBandSide of the width is left out at each side, which is the wall and the poster's edge and none of a screen's
// own things (the logo and the paper end at 611), and it is stretched for the rest, by up to kBandStretch: nothing
// on 16:9, a twentieth on 16:10. Bars only on a window further off than that.
constexpr float kBandTop = 65.0f / 480.0f, kBandBottom = kArtBottom / 480.0f;
constexpr float kBandSide = 28.0f / 640.0f, kBandStretch = 0.08f;
std::atomic<int64_t> g_front_end_ms{0};  // a front end screen last polled
std::atomic<int64_t> g_level_menu_ms{0};  // a menu in a level (the pause menu, a mission's map) last up

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The title screen is never seen (TitleScreen). Asked at each draw and not only by the pad's poll: on Windows it was
// still there for a second after START, flickering, while the game moved on to the next screen. So its own draw is
// skipped (sub_003145C0), and while it is the front end's screen no widget is drawn at all.
std::atomic<int64_t> g_title_drawn_ms{0};  // last drawn on purpose, for its picture to be saved, the window black

bool TitleWanted() { return NowMs() - g_title_drawn_ms.load(std::memory_order_relaxed) <= 250; }

bool TitleHidden() {
    uint32_t fe = R32(0x95F8F8), screens = fe ? R32(fe + 4) : 0;
    int32_t active = fe ? int32_t(R32(fe + 0xC)) : -1;
    uint32_t menu = screens && active >= 0 && active < 64 ? R32(screens + uint32_t(active) * 4) : 0;
    return menu && R32(menu) == 0x8297F0 && !TitleWanted();
}
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

// how much higher a widget (text or button prompt) is drawn than the game has it: the ones under the front end's art
bool g_foot_draw = false;  // one is being drawn, moved

float FootLift(uint32_t widget) {
    if (!app::Widescreen() || !R32(kFrontEnd)) return 0.0f;
    float y = RF(widget + 0x30);
    return y >= kArtBottom && y < 480.0f ? kFootLift : 0.0f;
}

uint32_t ActiveScreen(uint32_t manager_slot);

// how far right (less than 0: left) a button prompt under the art is drawn than the game has it, measured from its
// pieces (where it is drawn: a Bonus picture shown full screen moves them 200 right of the widget's own place).
// - the main menu (vtable 0x829B80) and Controller (0x829C48): in the bottom left corner (his ask), kPromptLeft
//   across, 16 inside the shown band's edge (kBandSide)
// - Bonus Materials' Characters, Weapons and Vehicles pages fill the right hand page down to the art's last rows
//   with the bio or the item's text: under the list on the left hand page (middle about 140 across), which ends
//   higher. Not while a picture is full screen (Weapons +0xAC, Vehicles +0x88): no text under it.
constexpr uint32_t kTextPageVtables[] = {0x829728, 0x828868, 0x829AB8};  // Characters, Weapons, Vehicles
constexpr float kPromptLeft = 44.0f, kLeftPageMiddle = 140.0f;
float FootShift(X86Context&, uint32_t widget) {
    if (FootLift(widget) <= 0.0f || R32(widget) != 0x82A450) return 0.0f;
    uint32_t screen = ActiveScreen(kFrontEnd), vt = screen ? R32(screen) : 0;
    bool corner = vt == 0x829B80 || vt == 0x829C48;  // the main menu (kMainMenuVtable), Controller
    bool text_page = std::find(std::begin(kTextPageVtables), std::end(kTextPageVtables), vt) !=
                         std::end(kTextPageVtables) &&
                     !(vt == 0x828868 && R8(screen + 0xAC)) && !(vt == 0x829AB8 && R8(screen + 0x88));
    if (!corner && !text_page) return 0.0f;
    uint32_t pieces = R32(widget + 0x80), count = std::min<uint32_t>(R32(widget + 0x78), 256);
    float left = 1e9f, right = -1e9f;
    for (uint32_t i = 0; pieces && i < count; i++) {
        float px = RF(pieces + i * 0x20 + 4), pw = RF(pieces + i * 0x20 + 0x10);
        if (!std::isfinite(px) || !std::isfinite(pw) || pw < 0) continue;
        left = std::min(left, px), right = std::max(right, px + pw);
    }
    if (right <= left) return 0.0f;
    return corner ? kPromptLeft - left : kLeftPageMiddle - (left + right) / 2;
}

// Before such a widget's draw (0x321160 or 0x30F900, ecx the widget): its black edge, the widget drawn in black a
// little to every side, and the widget left where it is to be drawn. FootEnd after the draw puts it back.
struct Foot {
    bool on = false;
    float y = 0;  // moved up by
    float x = 0;  // moved right by
};

// (a text draws from its place at +0x2C; a button prompt, vtable 0x82A450, from its laid out pieces: +0x78 of them at
// [+0x80], 0x20 bytes each with x and y at +4 and +8, drawn by 0x32BAB0)
void FootMove(uint32_t widget, float dx, float dy) {
    if (R32(widget) == 0x82A450) {
        uint32_t pieces = R32(widget + 0x80), count = std::min<uint32_t>(R32(widget + 0x78), 256);
        for (uint32_t i = 0; pieces && i < count; i++) {
            kernel::G<float>(pieces + i * 0x20 + 4) += dx;
            kernel::G<float>(pieces + i * 0x20 + 8) += dy;
        }
        return;
    }
    kernel::G<float>(widget + 0x2C) += dx;
    kernel::G<float>(widget + 0x30) += dy;
}

Foot FootBegin(X86Context& ctx, uint32_t widget, uint32_t draw) {
    Foot f;
    float lift = g_foot_draw ? 0.0f : FootLift(widget);
    if (lift <= 0.0f) return f;
    f.on = true;
    f.y = lift;
    f.x = FootShift(ctx, widget);
    uint32_t color = R32(widget + 0x50), ecx = ctx.ecx;
    g_foot_draw = true;
    W32(widget + 0x50, color & 0xFF000000);
    constexpr float kEdge = 1.1f, kSlant = 0.78f;
    constexpr float kAround[8][2] = {{kEdge, 0}, {-kEdge, 0}, {0, kEdge}, {0, -kEdge},
                                     {kSlant, kSlant}, {-kSlant, kSlant}, {kSlant, -kSlant}, {-kSlant, -kSlant}};
    for (const auto& d : kAround) {
        FootMove(widget, f.x + d[0], d[1] - lift);
        ctx.ecx = widget;
        kernel::CallGuest(ctx, draw, {});
        FootMove(widget, -f.x - d[0], lift - d[1]);
    }
    W32(widget + 0x50, color);
    FootMove(widget, f.x, -lift);
    ctx.ecx = ecx;
    return f;
}

void FootEnd(uint32_t widget, const Foot& f) {
    if (!f.on) return;
    FootMove(widget, -f.x, f.y);
    g_foot_draw = false;
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
    top -= FootLift(widget);  // where it is drawn
    left += FootShift(ctx, widget);
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

// The Bonus Materials pages (BonusPage) do their work in their own up and down (vtable +0x50 / +0x54, thiscall, the
// pad): a row's first item (0 until then) set to 1, its texts and its picture loaded. A row selected with +0x88
// alone kept item 0, so the page asked for files like vh3_0.dds that do not exist: a white picture, a white full
// screen and "BONUS_VEH_3_0_INFO" for its text. So on those pages the pointer moves the selection a row at a time
// through them, as the pad does.
constexpr uint32_t kUpSlot = 0x50, kDownSlot = 0x54;
void StepTo(X86Context& ctx, uint32_t menu, int16_t index) {
    uint32_t vtable = R32(menu), ecx = ctx.ecx;
    for (int i = 0; i < 64; i++) {
        int16_t at = R16s(menu + 0x22);
        if (at == index) break;
        ctx.ecx = menu;
        kernel::CallGuest(ctx, R32(vtable + (index < at ? kUpSlot : kDownSlot)), {0});
        if (R16s(menu + 0x22) == at) break;  // no further that way (a hidden row, or the page is full screen)
    }
    ctx.ecx = ecx;
}

void Select(X86Context& ctx, uint32_t menu, int16_t index) {
    if (BonusPage(menu)) {
        StepTo(ctx, menu, index);
        return;
    }
    uint32_t vtable = R32(menu), ecx = ctx.ecx;
    ctx.ecx = menu;
    if (R8(menu + 0x31)) kernel::CallGuest(ctx, R32(vtable + kSoundSlot), {});
    ctx.ecx = menu;
    kernel::CallGuest(ctx, R32(vtable + kSelectSlot), {uint32_t(int32_t(index)), 1});
    ctx.ecx = ecx;
}

// ---- the typewriter ----------------------------------------------------------------------------------------------

constexpr int kKeys = 36, kBackspace = 36, kShift = 37, kSpace = 38, kDone = 39;
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
    // The keys show capitals while the keyboard would type them: Shift held, or Caps Lock on (and small letters with
    // both). The screen's own shift key (+0x1A8 its state) is pressed when that changes, so a controller's use of
    // that key stands in between.
    static bool shown = false;
    static uint32_t screen = 0;
    if (screen != menu) screen = menu, shown = false;  // a new screen starts in small letters
    char c = input::TakeTyped();
    if (c) Type(ctx, menu, c);
    if (R32(menu) != kTypewriterVtable) return;  // Done or back closed it
    bool capitals = input::ShiftHeld() != input::CapsLockOn();
    if (capitals == shown) return;
    shown = capitals;
    if ((R8(menu + 0x1A8) != 0) == capitals) return;
    uint32_t ecx = ctx.ecx;
    ctx.ecx = menu;
    kernel::CallGuest(ctx, kTypewriterActivate, {uint32_t(kShift)});
    ctx.ecx = ecx;
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
    app::FrameView view;
    if (app::FrameBandView(p.width, p.height, view)) {
        // a part of the frame is shown (BlitFrame), not the whole of it as a fitted 16:9 picture
        if (p.x < view.x || p.x >= view.x + view.w || p.y < view.y || p.y >= view.y + view.h) return out;
        x = (view.u + (p.x - view.x) / view.w * view.uw) * 640.0f;
        y = (view.v + (p.y - view.y) / view.h * view.vh) * 480.0f;
    } else if (!game::menu::MapPointer(p.x, p.y, p.width, p.height, 640, 480, app::Widescreen(), x, y)) {
        return out;
    }
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
        if (!text || !WidgetBox(ctx, prompt, box)) continue;
        // across, where its pieces are drawn (FootMove): a page can move them from the widget's own place (a Bonus
        // page's picture full screen moves its prompts 200 right, and the box above missed them)
        uint32_t pieces = R32(prompt + 0x80), count = std::min<uint32_t>(R32(prompt + 0x78), 256);
        if (pieces && count) {
            float left = 1e9f, right = -1e9f, shift = FootShift(ctx, prompt);
            for (uint32_t i = 0; i < count; i++) {
                float px = RF(pieces + i * 0x20 + 4), pw = RF(pieces + i * 0x20 + 0x10);
                if (!std::isfinite(px) || !std::isfinite(pw) || pw < 0) continue;
                left = std::min(left, px + shift), right = std::max(right, px + pw + shift);
            }
            constexpr float kMargin = 3.0f;
            if (right > left) box.left = left - kMargin, box.right = right + kMargin;
        }
        if (!box.Contains(x, y)) continue;
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
        // each button with the words after it ("[A] Fullscreen  [B] Back"): the button whose words are under the pointer
        constexpr float kIconHalf = 1.1f;
        size_t pick = 0;
        for (size_t k = 1; k < icons.size(); k++)
            if (pos >= icons[k].first - kIconHalf - 0.5f) pick = k;
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
// settings (app::pc, menu.cpp): a first page of links (Display, Graphics, Keyboard and Mouse) and a page for each, the
// screen's title the page's. A page's settings wait for its Apply line (which leads back to the first page); B drops
// them and goes back to the first page, and from that out of the screen. A page's list scrolls at its ends. The game draws, highlights and
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
    float row_step;           // the lines this far apart (0: as the game has them)
    float action_shift;       // Reset and Apply this much lower
    float most_width;         // a value wider is drawn smaller
    float key_offset;         // a binding's two keys: each this far from the value's middle, and this wide at most
    float key_width;
};

// the Gameplay screen's lines this far apart, its own too (31 as the game has them): room for "Page n" under them
constexpr float kGameplayStep = 27.0f;

constexpr Layout kLayouts[] = {
    {0x828930, kFrontEnd, kOptionsVtable, 0, 5, {0, 1, 2, 3, 4, -1, -1}, {-1, -1, -1}, 0x3C, 0x50, 10, 5, 6,
     "gameplay", "PC OPTIONS", "PC Options", false, kGameplayStep, 8.0f, 140.0f, 44.0f, 84.0f},
    {0x82A388, kInGame, 0x8289F8, 1, 7, {0, 1, 4, 5, 7, 8, 9}, {2, 3, 6}, 0x38, 0x54, 14, 10, 11,
     "options", "PC OPTIONS", "PC Options", true, 28.0f, 0.0f, 150.0f, 48.0f, 90.0f},
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
    float own_action_y[2] = {};  // Reset's and Apply's labels
    uint32_t idle_color = 0, lit_color = 0;  // a value's colour on a line not selected, and on the selected one
    // a binding line: its two keys (strings), the key the selection is on (-1 none) and whether it waits for a key
    bool row_binding[kMostLines] = {}, row_waiting[kMostLines] = {};
    bool row_link[kMostLines] = {};  // a line that opens a page: no arrows
    uint32_t page = 0;               // app::pc's page serial the screen shows
    bool page_set = false;
    uint32_t own_apply_text = 0;     // the Apply and the Reset lines' own words (the page's on a page)
    uint32_t own_reset_text = 0;
    // under the list when it has more than one page: "Page n" with an arrow each side to the page before (0) and
    // after (1), each shown when there is one
    struct Mark {
        bool show = false;
        float x = 0, y = 0;
    } marks[2];
    struct Pager {
        bool show = false;
        uint32_t label = 0;  // Reset to Defaults' text widget, drawn again as the page's words
        uint32_t text = 0;   // "Page n"
        float x = 0, y = 0, scale[2] = {};
    } pager;
    // the screen the marks are on: PC Options, or the game's Gameplay screen with its own lines up, whose second page
    // (aim assist, hints, the HUD) is a PC Options page on the same screen
    uint32_t marks_screen = 0, marks_frame = 0;
    const Layout* marks_layout = nullptr;
    bool extra = false;       // up as the Gameplay screen's second page
    bool want_extra = false;  // asked for (down from its last line, or the mark clicked)
    // the second page's Apply and Exit: its settings applied, then the screen's own Apply and Exit for the first
    // page's, the selection put on it and A tapped once the press that asked is let go
    bool apply_exit = false;
    uint32_t tap_apply = 0, tap_frame = 0;
    const Layout* tap_layout = nullptr;
    int select_row = -1;      // a page of lines turned: the row the selection goes to, once the screen takes one
    uint8_t own_disabled[kMostLines] = {};  // a line past a short page's last is disabled: the selection skips it
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

uint32_t ArrowOf(uint32_t screen, const Layout& l, int a) {
    return a < l.arrow_count ? R32(screen + l.arrows + uint32_t(a) * 4) : 0;
}
uint32_t PcArrow(int a) { return ArrowOf(g_pcs.screen, *g_pcs.layout, a); }
bool MarksUp() { return g_pcs.marks_screen && g_pcs.marks_layout && Frame() - g_pcs.marks_frame <= 2; }
// the game's Gameplay screen with its own lines
bool GameplayUp() { return MarksUp() && !g_pcs.screen; }

// a long list goes in pages of the screen's lines: the first line of the last one
int LastTop(int rows, int lines) { return rows > 0 ? (rows - 1) / lines * lines : 0; }

// another page of the list, the selection on its first line (on its last going back)
void TurnPage(int dir) {
    g_pcs.top += dir * g_pcs.layout->lines;  // kept to a page each poll
    g_pcs.select_row = dir > 0 ? 0 : g_pcs.layout->lines - 1;
}

// the game's Gameplay screen with its own lines: they go kGameplayStep apart from the first, as on its second page,
// for the room under them. Where the game put them is kept for each screen, the first time it is seen.
struct GameplayOwn {
    float label[kMostLines], value[kMostLines], arrow[kMostLines][2][2], action[2];
};
std::unordered_map<uint32_t, GameplayOwn> g_gameplay_own;

void CloseUpGameplay(uint32_t menu, const Layout& l) {
    auto [it, fresh] = g_gameplay_own.try_emplace(menu);
    GameplayOwn& own = it->second;
    for (int i = 0; i < l.lines; i++) {
        uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
        uint32_t value = R32(menu + l.values + uint32_t(i) * 4);
        if (fresh) {
            own.label[i] = label ? RF(label + 0x30) : 0;
            own.value[i] = value ? RF(value + 0x30) : 0;
            for (int side = 0; side < 2; side++) {
                uint32_t arrow = ArrowOf(menu, l, i * 2 + side);
                own.arrow[i][side][0] = arrow ? RF(arrow + 0x14) : 0;
                own.arrow[i][side][1] = arrow ? RF(arrow + 0x18) : 0;
            }
        }
        if (fresh && i == 0)
            for (int k = 0; k < 2; k++) {  // Reset and Apply, lower by as much as on the second page
                uint32_t a = LineItem(menu, k ? l.apply_item : l.reset_item), al = a ? R32(a + 0x10) : 0;
                own.action[k] = al ? RF(al + 0x30) : 0;
            }
        if (i == 0)
            for (int k = 0; k < 2; k++) {
                uint32_t a = LineItem(menu, k ? l.apply_item : l.reset_item), al = a ? R32(a + 0x10) : 0;
                if (al && own.action[k] > 0) kernel::G<float>(al + 0x30) = own.action[k] + l.action_shift;
            }
        float shift = own.label[0] + kGameplayStep * float(i) - own.label[i];
        if (label) kernel::G<float>(label + 0x30) = own.label[i] + shift;
        if (value) kernel::G<float>(value + 0x30) = own.value[i] + shift;
        for (int side = 0; side < 2; side++) {
            uint32_t arrow = ArrowOf(menu, l, i * 2 + side);
            if (arrow && std::fabs(RF(arrow + 0x18) - (own.arrow[i][side][1] + shift)) > 0.01f)
                MovePicture(arrow, own.arrow[i][side][0], own.arrow[i][side][1] + shift);
        }
    }
}

// the in-game Options screen with its own lines: Reset and Apply a little lower, for the room "Page 1" takes under
// its ten lines. Where the game put them is kept for each screen, as above.
constexpr float kOptionsActionShift = 10.0f;

void LowerOptionsActions(uint32_t menu, const Layout& l) {
    auto [it, fresh] = g_gameplay_own.try_emplace(menu);
    for (int k = 0; k < 2; k++) {
        uint32_t a = LineItem(menu, k ? l.apply_item : l.reset_item), al = a ? R32(a + 0x10) : 0;
        if (fresh) it->second.action[k] = al ? RF(al + 0x30) : 0;
        if (al && it->second.action[k] > 0) kernel::G<float>(al + 0x30) = it->second.action[k] + kOptionsActionShift;
    }
}

// "Page n" under the list, between its last line and Reset to Defaults, in Reset's letters a little smaller, with
// an arrow each side where there is a page that way. page counts from 1; 0 shows none
void BottomMarks(X86Context& ctx, uint32_t menu, const Layout& l, int page, bool before, bool after) {
    uint32_t item = LineItem(menu, l.reset_item), label = item ? R32(item + 0x10) : 0;
    uint32_t arrow = ArrowOf(menu, l, (l.lines - 1) * 2);
    game::menu::Rect box;
    g_pcs.marks[0].show = g_pcs.marks[1].show = g_pcs.pager.show = false;
    if (page <= 0 || !label || !arrow || !WidgetBox(ctx, label, box)) return;
    float x = (box.left + box.right) / 2, y = (RF(arrow + 0x18) + box.top) / 2;
    if (!g_pcs.screen && l.manager == kInGame) {
        // the in-game Options screen's own lines (their arrows are not where +0x18 says): under the last one's words
        uint32_t last = LineItem(menu, l.items[l.lines - 1]), last_label = last ? R32(last + 0x10) : 0;
        game::menu::Rect last_box;
        if (!last_label || !WidgetBox(ctx, last_label, last_box)) return;
        y = (last_box.bottom + box.top) / 2;
    }
    // the words' size: Reset's widget with them, measured and put back
    constexpr float kSmaller = 0.7f;
    uint32_t text = PcString("Page " + std::to_string(page));
    uint32_t own_text = R32(label + 0x1C);
    float own_scale[2] = {RF(label + 0x38), RF(label + 0x3C)};
    float scale[2] = {own_scale[0] * kSmaller, own_scale[1] * kSmaller};
    W32(label + 0x1C, text);
    kernel::G<float>(label + 0x38) = scale[0];
    kernel::G<float>(label + 0x3C) = scale[1];
    float w = WidgetMethod(ctx, kWidgetWidth, label), h = WidgetMethod(ctx, kWidgetHeight, label);
    W32(label + 0x1C, own_text);
    kernel::G<float>(label + 0x38) = own_scale[0];
    kernel::G<float>(label + 0x3C) = own_scale[1];
    if (!std::isfinite(w) || !std::isfinite(h) || w <= 0 || h <= 0) return;
    // its place as the widget keeps it (x the middle as Reset's; y by the widget's own alignment)
    uint8_t align = R8(label + 0x5C);
    float anchor_y = (align & 0x80) ? y + h / 2 : (align & 0x40) ? y - h / 2 : y;
    g_pcs.pager = {true, label, text, RF(label + 0x2C) + (x - (box.left + box.right) / 2), anchor_y, {scale[0], scale[1]}};
    constexpr float kGap = 14.0f;  // from the words to an arrow's middle
    if (before) g_pcs.marks[0] = {true, x - w / 2 - kGap, y};
    if (after) g_pcs.marks[1] = {true, x + w / 2 + kGap, y};
}

// Reset to Defaults' text about to be drawn: "Page n" first, with it, where the pager is
bool g_pager_draw = false;

void PcScreenDrawPager(X86Context& ctx, uint32_t widget) {
    const PcScreen::Pager p = g_pcs.pager;
    if (g_pager_draw || !MarksUp() || !p.show || widget != p.label || !p.text) return;
    uint32_t text = R32(widget + 0x1C), color = R32(widget + 0x50);
    float x = RF(widget + 0x2C), y = RF(widget + 0x30), sx = RF(widget + 0x38), sy = RF(widget + 0x3C);
    kernel::G<uint16_t>(p.text + 4) = 0x4000;
    W32(widget + 0x1C, p.text);
    if (g_pcs.idle_color) W32(widget + 0x50, g_pcs.idle_color);
    kernel::G<float>(widget + 0x2C) = p.x;
    kernel::G<float>(widget + 0x30) = p.y;
    kernel::G<float>(widget + 0x38) = p.scale[0];
    kernel::G<float>(widget + 0x3C) = p.scale[1];
    ctx.ecx = widget;
    g_pager_draw = true;
    kernel::CallGuest(ctx, 0x321160, {});
    g_pager_draw = false;
    W32(widget + 0x1C, text);
    W32(widget + 0x50, color);
    kernel::G<float>(widget + 0x2C) = x;
    kernel::G<float>(widget + 0x30) = y;
    kernel::G<float>(widget + 0x38) = sx;
    kernel::G<float>(widget + 0x3C) = sy;
    ctx.ecx = widget;
}

void LeavePcScreen() {
    const Layout& l = *g_pcs.layout;
    uint32_t menu = g_pcs.screen;
    for (int i = 0; i < l.lines; i++) {
        uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
        if (label && g_pcs.own_labels[i]) {
            W32(label + 0x1C, g_pcs.own_labels[i]);
            kernel::G<float>(label + 0x30) = g_pcs.own_label_y[i];
        }
        if (item) kernel::G<uint8_t>(item + 0x16) = g_pcs.own_disabled[i];
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
    for (int k = 0; k < 2; k++) {
        uint32_t item = LineItem(menu, k ? l.apply_item : l.reset_item), label = item ? R32(item + 0x10) : 0;
        if (label && g_pcs.own_action_y[k] > 0) kernel::G<float>(label + 0x30) = g_pcs.own_action_y[k];
    }
    if (uint32_t item = LineItem(menu, l.apply_item), label = item ? R32(item + 0x10) : 0; label && g_pcs.own_apply_text)
        W32(label + 0x1C, g_pcs.own_apply_text);
    if (uint32_t item = LineItem(menu, l.reset_item), label = item ? R32(item + 0x10) : 0; label && g_pcs.own_reset_text)
        W32(label + 0x1C, g_pcs.own_reset_text);
    for (auto [title, text] : g_pcs.titles) W32(title + 0x1C, text);
    g_pcs.titles.clear();
    {
        std::lock_guard lock(g_drawn_lock);
        g_pcs.want.clear();
    }
    app::pc::Leave(g_pcs.applied);
    LOGR("PC Options closed%s", g_pcs.applied ? ", applied" : "");
    g_pcs.extra = false;
    g_pcs.marks_screen = 0;
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
    g_pcs.page_set = false;
    g_pcs.extra = false;
    g_pcs.apply_exit = false;
    {
        uint32_t item = LineItem(menu, l.apply_item), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_apply_text = label ? R32(label + 0x1C) : 0;
        item = LineItem(menu, l.reset_item), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_reset_text = label ? R32(label + 0x1C) : 0;
    }
    float first_y = 0, step_y = 0;
    int16_t selected = R16s(menu + 0x22);
    g_pcs.idle_color = 0;
    for (int i = 0; i < l.lines; i++) {
        uint32_t item = LineItem(menu, l.items[i]), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_labels[i] = label ? R32(label + 0x1C) : 0;
        g_pcs.own_disabled[i] = item ? R8(item + 0x16) : 0;
        g_pcs.row_binding[i] = g_pcs.row_waiting[i] = g_pcs.row_link[i] = false;
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
    for (int k = 0; k < 2; k++) {
        uint32_t item = LineItem(menu, k ? l.apply_item : l.reset_item), label = item ? R32(item + 0x10) : 0;
        g_pcs.own_action_y[k] = label ? RF(label + 0x30) : 0;
        // the Gameplay screen's own lines have them lower already: from where the game put them
        if (auto it = g_gameplay_own.find(menu); it != g_gameplay_own.end() && it->second.action[k] > 0)
            g_pcs.own_action_y[k] = it->second.action[k];
        if (label) kernel::G<float>(label + 0x30) = g_pcs.own_action_y[k] + l.action_shift;
    }
    if (l.row_step > 0) step_y = l.row_step;
    if ((l.close_up || l.row_step > 0) && step_y > 0) {
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
        // the line selected, then A tapped once the screen is still again (a tap while its highlight still moves is
        // lost), again if it did not take
        if (g_pcs.tapped && poll - g_pcs.tapped_at > 40) g_pcs.tapped = false;
        if (menu && R32(menu) == want->pick_vtable && !g_pcs.tapped && !R8(menu + 0x30) && poll - g_pcs.asked >= 6) {
            if (R16s(menu + 0x22) != want->pick_item) {
                Select(ctx, menu, int16_t(want->pick_item));
            } else {
                TapA();
                g_pcs.tapped = true;
                g_pcs.tapped_at = poll;
            }
        } else if (menu && R32(menu) == want->vtable && g_pcs.tapped) {
            TakePcScreen(menu, *want);
        }
    }
    if (!g_pcs.screen) {
        // the game's Gameplay screen, and in a level its Options screen, which has the same lines: "Page 1" and a
        // mark under its lines, and its second page (aim assist, hints, the HUD) when that is asked for
        const Layout* own = nullptr;
        for (const Layout& k : kLayouts)
            if (manager == R32(k.manager)) own = &k;
        if (!own) return;
        const Layout& gl = *own;
        bool gameplay = menu && R32(menu) == gl.vtable && !g_pcs.want_layout;
        if (!gameplay) g_pcs.want_extra = false;
        if (!gameplay) return;
        if (gl.manager == kFrontEnd) CloseUpGameplay(menu, gl);
        else LowerOptionsActions(menu, gl);
        BottomMarks(ctx, menu, gl, 1, false, true);  // its own lines are its first page
        g_pcs.marks_screen = menu;
        g_pcs.marks_layout = &gl;
        g_pcs.marks_frame = frame;
        if (g_pcs.want_extra && !R8(menu + 0x30)) {
            g_pcs.want_extra = false;
            TakePcScreen(menu, gl);
            g_pcs.extra = true;
            app::pc::OpenExtra();
            LOGR("Gameplay: the second page");
        }
        return;
    }
    if (manager != R32(g_pcs.layout->manager)) return;
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
    if (g_pcs.extra && app::pc::ExtraDone()) {  // the Gameplay screen's own lines again, on its last
        bool apply_exit = g_pcs.apply_exit;
        LeavePcScreen();
        if (apply_exit) {  // or on its Apply and Exit, which OptionsPad presses
            Select(ctx, menu, int16_t(l.apply_item));
            g_pcs.tap_apply = menu;
            g_pcs.tap_frame = frame;
            g_pcs.tap_layout = &l;
            LOGR("Gameplay: the second page applied, now the screen's own Apply and Exit");
        } else {
            Select(ctx, menu, int16_t(l.items[l.lines - 1]));
            LOGR("Gameplay: the first page");
        }
        return;
    }
    g_pcs.marks_screen = menu;
    g_pcs.marks_layout = &l;
    g_pcs.marks_frame = frame;
    // another page: the list from the line the selection goes to, once the screen takes a selection
    if (uint32_t page = app::pc::PageSerial(); !g_pcs.page_set || page != g_pcs.page) {
        std::vector<app::pc::Line> lines = app::pc::Lines(-1);
        int rows = 0;
        while (rows < int(lines.size()) && !lines[size_t(rows)].action) rows++;
        int focus = std::clamp(app::pc::Focus(), 0, std::max(0, rows - 1));
        g_pcs.top = focus / l.lines * l.lines;
        g_pcs.select_row = -1;
        int16_t item = int16_t(l.items[focus - g_pcs.top]);
        // (as the screen comes up too: it keeps the selection it was left with, which may be a line with no setting)
        if (R16s(menu + 0x22) == item) {
            g_pcs.page = page;
            g_pcs.page_set = true;
        } else if (!R8(menu + 0x30)) {
            Select(ctx, menu, item);
        }
    }
    int selected = R16s(menu + 0x22), row = -1;
    for (int i = 0; i < l.lines; i++)
        if (l.items[i] == selected) row = i;
    std::vector<app::pc::Line> lines = app::pc::Lines(row >= 0 ? g_pcs.top + row : -1);
    int rows = 0;
    while (rows < int(lines.size()) && !lines[size_t(rows)].action) rows++;
    g_pcs.top = std::clamp(g_pcs.top, 0, LastTop(rows, l.lines)) / l.lines * l.lines;
    if (g_pcs.select_row >= 0 && g_pcs.page_set && !R8(menu + 0x30)) {
        // (a short last page: its last line with a setting)
        int to = std::clamp(g_pcs.select_row, 0, std::max(0, std::min(l.lines, rows - g_pcs.top) - 1));
        if (R16s(menu + 0x22) != l.items[to]) Select(ctx, menu, int16_t(l.items[to]));
        g_pcs.select_row = -1;
    }
    // the Apply line reads as the page's on a page (Apply, not Apply and Exit), in the case the screen writes its own
    for (int k = 0; k < 2; k++) {
        uint32_t item = LineItem(menu, k ? l.apply_item : l.reset_item), label = item ? R32(item + 0x10) : 0;
        uint32_t own = k ? g_pcs.own_apply_text : g_pcs.own_reset_text;
        if (!label || !own || rows + k >= int(lines.size())) continue;
        std::string words = lines[size_t(rows + k)].label;
        // Reset to Defaults stays the screen's own line; another action (the page that asks) is written over it
        bool ours = app::pc::Subpage() && (k || words != "Reset to Defaults");
        if (k && g_pcs.extra && words == "Apply and Exit") ours = false;  // the screen's own Apply and Exit
        if (std::isupper(uint8_t(R8(own + 7))))
            for (char& c : words) c = char(std::toupper(uint8_t(c)));
        W32(label + 0x1C, ours ? PcString(words) : own);
    }
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
            if (item) kernel::G<uint8_t>(item + 0x16) = pl && !pl->note ? g_pcs.own_disabled[i] : 1;
            if (value) W32(value + 0x1C, g_pcs.want[value] = PcString(v));
            g_pcs.row_binding[i] = pl && pl->binding;
            g_pcs.row_link[i] = !pl || pl->link || pl->note;  // nor has a line past the page's last
            g_pcs.row_waiting[i] = pl && pl->binding && pl->waiting;
            g_pcs.row_slot[i] = pl && pl->binding ? pl->slot : -1;
            for (int k = 0; k < 2; k++) g_pcs.row_keys[i][k] = pl && pl->binding ? PcString(pl->keys[k]) : 0;
        }
    }
    // more pages before or after: marks under the list. The Gameplay screen's second page has the screen's own
    // lines before it (not on the page that asks, whose first line is words)
    {
        // the Gameplay screen's second page counts the screen's own lines as its first; the page that asks has none
        int before = g_pcs.extra ? 1 : 0, pages = LastTop(rows, l.lines) / l.lines + 1 + before;
        int page = g_pcs.top / l.lines + 1 + before;
        bool asking = !lines.empty() && lines[0].note;
        BottomMarks(ctx, menu, l, asking || pages < 2 ? 0 : page, page > 1, g_pcs.top + l.lines < rows);
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
        float middle = RF(value + 0x2C);
        constexpr float kGap = 14.0f;  // from the value's end to an arrow's middle
        // no wider than the line has room for: the left arrow stays clear of a long label
        float most = l.most_width;
        game::menu::Rect label_box;
        uint32_t row_item = LineItem(menu, l.items[i]), row_label = row_item ? R32(row_item + 0x10) : 0;
        if (row_label && WidgetBox(ctx, row_label, label_box)) {
            constexpr float kArrowHalf = 7.0f, kLeast = 36.0f;
            most = std::clamp((middle - label_box.right - kGap - kArrowHalf) * 2, kLeast, most);
        }
        if (width > most) {
            float shrink = most / width;
            kernel::G<float>(value + 0x38) = g_pcs.own_scales[i][0] * shrink;
            kernel::G<float>(value + 0x3C) = g_pcs.own_scales[i][1] * shrink;
            width = most;
        }
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

// the first line's right arrow about to be drawn (ecx): first the pager's arrows, the same picture a little smaller,
// turned to point left for the page before
bool g_mark_draw = false;

void PcScreenDrawMarks(X86Context& ctx, uint32_t widget) {
    if (g_mark_draw || !MarksUp() || widget != ArrowOf(g_pcs.marks_screen, *g_pcs.marks_layout, 1)) return;
    uint32_t pictures = R32(widget + 0x20), count = std::min<uint32_t>(R32(widget + 0x28), 4);
    if (!pictures) return;
    constexpr float kSize = 0.8f;
    for (int k = 0; k < 2; k++) {
        const PcScreen::Mark mark = g_pcs.marks[k];
        if (!mark.show) continue;
        float kept[4][4][2] = {};
        for (uint32_t p = 0; p < count; p++) {
            uint32_t corners = R32(pictures + p * 4) + 0x40;
            if (corners == 0x40) continue;
            float cx = 0, cy = 0;
            for (uint32_t c = 0; c < 4; c++) {
                cx += (kept[p][c][0] = RF(corners + c * 0x14)) / 4;
                cy += (kept[p][c][1] = RF(corners + c * 0x14 + 4)) / 4;
            }
            for (uint32_t c = 0; c < 4; c++) {
                float dx = (kept[p][c][0] - cx) * kSize, dy = (kept[p][c][1] - cy) * kSize;
                kernel::G<float>(corners + c * 0x14) = mark.x + (k ? dx : -dx);
                kernel::G<float>(corners + c * 0x14 + 4) = mark.y + (k ? dy : -dy);
            }
        }
        ctx.ecx = widget;
        g_mark_draw = true;
        kernel::CallGuest(ctx, 0x316DD0, {});
        g_mark_draw = false;
        for (uint32_t p = 0; p < count; p++) {
            uint32_t corners = R32(pictures + p * 4) + 0x40;
            if (corners == 0x40) continue;
            for (uint32_t c = 0; c < 4; c++) {
                kernel::G<float>(corners + c * 0x14) = kept[p][c][0];
                kernel::G<float>(corners + c * 0x14 + 4) = kept[p][c][1];
            }
        }
    }
    ctx.ecx = widget;
}

// a click at x, y: on a mark, the next or the last page of the list; the Gameplay screen goes to its second page,
// and from its first page of lines back to the screen's own. True when taken.
bool PcScreenClickMark(float x, float y) {
    if (!MarksUp()) return false;
    constexpr float kReach = 11.0f;
    for (int k = 0; k < 2; k++) {
        const PcScreen::Mark& mark = g_pcs.marks[k];
        if (!mark.show || std::fabs(x - mark.x) > kReach || std::fabs(y - mark.y) > kReach) continue;
        if (!g_pcs.screen) g_pcs.want_extra = k == 1;
        else if (!k && g_pcs.extra && g_pcs.top == 0) app::pc::Back();
        else TurnPage(k ? 1 : -1);
        return true;
    }
    return false;
}

// a picture about to be drawn: a slider of a line put aside (at its row, between the arrows) is not, nor a binding
// line's arrows; the other arrows are
bool PcScreenHidesPicture(uint32_t widget) {
    if (!PcScreenUp() || g_mark_draw) return false;
    const Layout& l = *g_pcs.layout;
    for (int a = 0; a < l.lines * 2; a++)
        if (PcArrow(a) == widget) return g_pcs.row_binding[a / 2] || g_pcs.row_link[a / 2];  // keys to click, or a page
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
    if (row < 0 || !(g_pcs.row_binding[row] || g_pcs.row_link[row])) return false;
    uint32_t value = R32(menu + l.values + uint32_t(row) * 4);
    if (!value) return false;
    if (R16s(menu + 0x22) != item) {
        if (R8(menu + 0x30)) return true;  // locked: not yet
        Select(ctx, menu, int16_t(item));
    }
    int line = g_pcs.top + row;
    if (g_pcs.row_link[row]) {  // a page's line opens it wherever it is clicked
        if (!right) app::pc::Press(line);
        return true;
    }
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
    auto title = [&](uint32_t own) {
        std::string words = app::pc::Title();
        if (std::isupper(uint8_t(R8(own + 7))))
            for (char& c : words) c = char(std::toupper(uint8_t(c)));
        return PcString(words);
    };
    if (auto it = g_pcs.titles.find(widget); !want && it != g_pcs.titles.end()) want = title(it->second);
    if (!want && text) {
        std::string t;
        for (uint32_t k = 0; k < 16; k++) {
            char c = char(R8(text + 6 + k));
            if (!c) break;
            t += char(std::tolower(uint8_t(c)));
        }
        // the title, above the lines (a menu's own line of the same name, drawn as it slides away, stays)
        if (t == l.title && RF(widget + 0x30) < 110.0f) {
            want = title(text);
            g_pcs.titles.emplace(widget, text);
        }
    }
    if (want) {
        kernel::G<uint16_t>(want + 4) = 0x4000;
        W32(widget + 0x1C, want);
    }
}

// the pad as the game is about to see it, while the Gameplay screen shows its own lines: down from the last of them
// asks for its second page. True when the screen is up.
bool GameplayPad(input::Pad& pad) {
    static bool was = false, hidden = false;
    if (!GameplayUp()) {
        was = hidden = false;
        return false;
    }
    constexpr int16_t kStick = 16000;
    const Layout& l = *g_pcs.marks_layout;
    uint32_t menu = g_pcs.marks_screen;
    bool down = (pad.buttons & 2) || pad.ly < -kStick;
    if (down && !was && !R8(menu + 0x30) && R16s(menu + 0x22) == l.items[l.lines - 1]) {
        g_pcs.want_extra = true;
        hidden = true;
    }
    was = down;
    hidden = hidden && down;
    if (hidden) {
        pad.buttons &= uint16_t(~2u);
        if (pad.ly < 0) pad.ly = 0;
    }
    return true;
}

// the pad as the game is about to see it, while PC Options is up: our lines' left, right, A and X are ours, up and
// down past a page's ends turn it, and a key box waiting for a key has every button. True when it is up.
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
        if ((edge & kUp) && row == 0 && g_pcs.top > 0) {  // up from a page's first line: the page before, its last
            TurnPage(-1);
            hidden |= kUp;
        } else if ((edge & kUp) && row == 0 && g_pcs.extra) {  // up from the second page's first line: the first page
            app::pc::Back();
            hidden |= kUp;
        } else if ((edge & kDown) && row == l.lines - 1 && g_pcs.top + l.lines < rows) {  // and down from its last
            TurnPage(1);
            hidden |= kDown;
        }
        if (row >= 0 && line < rows) {
            if (edge & (kLeft | kRight)) app::pc::Step(line, (edge & kLeft) ? -1 : 1);
            if (edge & kA) app::pc::Press(line);
            if (edge & kX) app::pc::Clear(line);
            hidden |= edge & (kLeft | kRight | kA | kX);
        } else if (selected == l.reset_item && (edge & kA)) {  // Reset to Defaults: ours
            app::pc::Press(rows);
            hidden |= kA;
        } else if (selected == l.apply_item && (edge & kA)) {
            // Apply on a page: ours, back on the first page. Apply on the first page: the game leaves, we apply
            if (app::pc::Subpage()) {
                // the Gameplay screen's second page: its Apply and Exit is the whole screen's
                g_pcs.apply_exit = g_pcs.extra && rows + 1 < int(lines.size()) &&
                                   lines[size_t(rows + 1)].label == "Apply and Exit";
                app::pc::Press(rows + 1);
                hidden |= kA;
            } else {
                g_pcs.applied = true;
            }
        }
        if (edge & kB) {  // from a page to the first one, from that out of the screen
            if (app::pc::Back()) hidden |= kB;
            else g_pcs.applied = false;
        }
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

// a mission's start screen, its level loaded ("Press A to start the game"): one of the in-game manager's screens 2 to 5,
// picked by the mission (0x27E395, on [0x8DCE48]: 5 the prologue's, vtable 0x8295A0, then 3, 4 and 2), each told it is
// done by its +0x38 set to 1
bool StartPrompt();

bool MissionStart(uint32_t menu) {
    uint32_t in_game = R32(kInGame);
    uint32_t screens = in_game ? R32(in_game + 4) : 0;
    if (!menu || !screens || !R8(menu + 0x38)) return false;
    for (uint32_t i = 2; i <= 5; i++)
        if (R32(screens + i * 4) == menu) return true;
    return false;
}

// The start screen's text is given once, when its level has loaded (0x27E3D2: the screen's prompt, vtable +0x6C, the
// key "FEMENU_XBOX_LEVEL_LOADING_DONE"), and the string table entry says it for the input in use then
// (src/game/pc_texts.cpp). A switch between pad and keyboard on the screen gives the prompt the key again.
constexpr uint32_t kLoadingDoneKey = 0x82BCD4;
constexpr struct {
    uint32_t screen, prompt;  // the in-game manager's screen and the field holding its prompt
} kStartPrompts[] = {{5, 0x58}, {3, 0x124}, {4, 0xA8}, {2, 0x10C}};

void RefreshStartText(X86Context& ctx, uint32_t menu, bool keyboard) {
    static uint32_t last_menu = 0;
    static bool last_keyboard = false;
    if (menu == last_menu && keyboard == last_keyboard) return;
    bool first = menu != last_menu;
    last_menu = menu;
    last_keyboard = keyboard;
    if (first) return;  // just set by the game itself, for this input
    uint32_t screens = R32(R32(kInGame) + 4);
    for (const auto& s : kStartPrompts) {
        if (R32(screens + s.screen * 4) != menu) continue;
        uint32_t prompt = R32(menu + s.prompt);
        if (!prompt) return;
        uint32_t ecx = ctx.ecx;
        ctx.ecx = prompt;
        kernel::CallGuest(ctx, R32(R32(prompt) + 0x6C), {kLoadingDoneKey});
        ctx.ecx = ecx;
        return;
    }
}

void PlayMoveSound(X86Context& ctx, uint32_t menu) {
    uint32_t ecx = ctx.ecx;
    ctx.ecx = menu;
    kernel::CallGuest(ctx, R32(R32(menu) + kSoundSlot), {});
    ctx.ecx = ecx;
}

// The title screen (the squad, the logo and "Press START", vtable 0x8297F0) is not a screen any more (his ask): START
// is pressed for it in the poll that finds it up, as a player would at once, so the game leaves it by its own way
// and nothing of its is skipped but the drawing (TitleHidden, sub_003145C0). Its picture is the boot splash instead
// (app::kGame.splash, a file beside the exe that the SDK shows while the game starts). That file is the player's
// own, made once: while there is none the title screen is drawn until it has faded in, with the window kept black
// (game::UpdateFrameBand), and the frame's rows with its art (94 to 384) are saved as the splash.
constexpr uint32_t kTitleVtable = 0x8297F0;
constexpr float kTitleTop = 92.0f / 480.0f, kTitleBottom = 386.0f / 480.0f;
constexpr uint32_t kSplashWidth = 960;

void TitleScreen() {
    enum { kNew, kFading, kSaving, kLeave };
    static int state = kNew;
    static int64_t since = 0, tapped = 0;
    constexpr int64_t kFadeMs = 1500, kSaveMs = 300, kTapMs = 150;
    int64_t now = NowMs();
    if (state == kNew) {
        std::error_code ec;
        bool have = false;
        for (const wchar_t* ext : {L".bmp", L".png"}) {
            std::filesystem::path file = app::ExeDir() / app::kGame.splash;
            file += ext;
            have |= std::filesystem::exists(file, ec);
        }
        state = have ? kLeave : kFading;
        since = now;
    }
    if (state == kFading && now - since >= kFadeMs) {
        std::filesystem::path file = app::ExeDir() / app::kGame.splash;
        file += L".bmp";
        gpu::SaveFrame({file, kTitleTop, kTitleBottom, kSplashWidth});
        LOGR("title screen: its picture is saved as the boot splash");
        state = kSaving, since = now;
    }
    if (state == kSaving && now - since >= kSaveMs) state = kLeave;
    if (state != kLeave) {
        g_title_drawn_ms.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - tapped < kTapMs) return;
    input::MenuTap(0x10);  // START
    tapped = now;
}

void MenuPointer(X86Context& ctx, uint32_t manager) {
    uint32_t screens = R32(manager + 4);
    int32_t active = int32_t(R32(manager + 0xC));
    uint32_t menu = screens && active >= 0 ? R32(screens + uint32_t(active) * 4) : 0;
    uint32_t frame = Frame();
    if (manager == R32(kFrontEnd) && menu) g_front_end_ms.store(NowMs(), std::memory_order_relaxed);
    // one of a level's screens is up (a mission's map as it loads): the front end's fit goes at once, where it
    // stayed a quarter of a second into the map and the map then changed size
    else if (manager != R32(kFrontEnd) && menu) g_front_end_ms.store(0, std::memory_order_relaxed);
    {
        static uint32_t shown[2] = {};
        int m = manager == R32(kFrontEnd) ? 0 : manager == R32(kInGame) ? 1 : -1;
        uint32_t vt = menu ? R32(menu) : 0;
        if (m >= 0 && vt != shown[m]) LOGI("menu screen %d: %08x", m, shown[m] = vt);  // for the harness's scripts
    }
    if (menu && manager == R32(kFrontEnd) && R32(menu) == kTitleVtable) TitleScreen();
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
    } else if (items.empty() && manager != R32(kFrontEnd) && !MissionStart(menu)) {
        return;  // in a level without a menu
    }
    if (MissionStart(menu)) RefreshStartText(ctx, menu, StartPrompt());
    g_pointer_frame = frame;
    g_cursor = true;
    // a menu with something to point at, in a level (the pause menu, a mission's map, a dialog): play polls a
    // screen with nothing shown and never comes here
    if (manager != R32(kFrontEnd)) g_level_menu_ms.store(NowMs(), std::memory_order_relaxed);
    input::SetMenuInput(true);
    Pointer pointer = TakePointer();
#ifdef _WIN32
    if (app::MenuOpen()) return;  // the settings panel has the mouse
#endif
    if (PcScreenUp() && app::pc::Capturing()) return;  // a key box waits for its key: the pointer moves nothing
    if (PcScreenPending()) return;  // PC Options asked for: the menu under it takes no hover or click until it is up
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
    // right of a label on its line: the row's value (not on a Bonus page, whose lines have none: a click on its
    // picture's arrows took the list's line at that height)
    float value_x = -1;
    if (!hit && !BonusPage(menu)) {
        for (const ItemBox& b : items)
            if (x >= b.box.right && y >= b.box.top && y < b.box.bottom && nearer(b)) hit = &b;
        // only a row: an item with left/right neighbours is a grid's (the typewriter's keys)
        uint32_t row = hit ? R32(R32(menu + 4) + uint32_t(hit->index) * 4) : 0;
        if (row && (R16s(row + 0xC) >= 0 || R16s(row + 0xE) >= 0)) hit = nullptr;
        if (hit && (value_x = ValueMiddle(ctx, items, *hit)) < 0) hit = nullptr;
    }
    if (pointer.left_clicks && menu == g_pcs.marks_screen && PcScreenClickMark(x, y)) return;
    // PC Options: a binding line's keys
    if ((pointer.left_clicks || pointer.right_clicks) && hit && value_x >= 0 &&
        PcScreenClick(ctx, menu, hit->index, x, pointer.right_clicks > 0))
        return;
    // PC OPTIONS under the Options menu's items: its box, and the gap above it up to halfway to SOUND (whose own box
    // reaches down over it)
    if (options && menu == g_pc.screen && PcEntryUp()) {
        game::menu::Rect box, above;
        bool on_pc = WidgetBox(ctx, g_pc.widget, box) && box.Contains(x, y);
        if (!on_pc && WidgetBox(ctx, g_pc.widget, box) && WidgetBox(ctx, g_pc.anchor, above) && x >= box.left &&
            x < box.right && y < box.top && y >= (above.top + above.bottom + box.top + box.bottom) / 4)
            on_pc = true;
        if (on_pc) {
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
        hover_pending = R16s(menu + 0x22) != hit->index;  // (a Bonus page steps a row at a time, StepTo)
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
    // Bonus Materials and its pages (kBonusPageVtables): the page on the right shows a movie or picture with an arrow
    // each side of it (about 270-291 and 589-591 across, 230-238 down in 640x480), stepped by left and right: a click
    // on that page left or right of its middle steps it
    if (pointer.left_clicks && !on_item && BonusPage(menu) && x > 250.0f && y > 120.0f && y < 340.0f) {
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
    else if (pointer.wheel && PcScreenUp())
        TurnPage(pointer.wheel > 0 ? -1 : 1);  // a page a notch (kept to the list each poll)
    else if (pointer.wheel)
        TapDpad(pointer.wheel > 0 ? kDpadUp : kDpadDown);
}

}  // namespace

namespace game {

void UpdateFrameBand() {
    constexpr int64_t kLately = 250;  // ms: a movie or a level's load polls no screen, and has the whole frame
    bool front_end = NowMs() - g_front_end_ms.load(std::memory_order_relaxed) <= kLately;
    // and the front end has a screen up now: on the frame it hands over to a mission's map it was polled a moment
    // before, and the map's first frame was shown with the menus' fit, a size it never has again
    uint32_t fe = R32(kFrontEnd), fe_screens = fe ? R32(fe + 4) : 0;
    int32_t fe_active = fe ? int32_t(R32(fe + 0xC)) : -1;
    bool fe_screen = fe_screens && fe_active >= 0 && R32(fe_screens + uint32_t(fe_active) * 4);
    uint32_t in_game = R32(kInGame), ig_screens = in_game ? R32(in_game + 4) : 0;
    int32_t ig_active = in_game ? int32_t(R32(in_game + 0xC)) : -1;
    bool start = ig_screens && ig_active >= 2 && ig_active <= 5 && MissionStart(R32(ig_screens + uint32_t(ig_active) * 4));
    bool band = app::Widescreen() && front_end && fe_screen && !start;
    // every menu stays at 60 frames a second at most, whatever the frame rate setting: the setting is for play, and
    // at several hundred a second the pause menu's highlight and clicks went to the wrong line
    bool level_menu = NowMs() - g_level_menu_ms.load(std::memory_order_relaxed) <= kLately;
    if (front_end || level_menu) app::LimitNextFrame(60);
    if (NowMs() - g_title_drawn_ms.load(std::memory_order_relaxed) <= kLately) {
        // the title screen is being drawn for its picture only: the window shows the frame's first rows, which are black
        app::SetFrameBand(0.0f, 60.0f / 480.0f);
        return;
    }
    app::SetFrameBand(band ? kBandTop : 0.0f, band ? kBandBottom : 1.0f, kBandSide, kBandStretch);
}

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
    GameplayPad(pad);
    if (uint32_t m = g_pcs.tap_apply) {  // the second page applied: the screen's own Apply and Exit
        if (Frame() - g_pcs.tap_frame > 120 || ActiveScreen(g_pcs.tap_layout->manager) != m) {
            g_pcs.tap_apply = 0;
        } else if (held_a) {
            pad.analog[0] = 0;  // the press that applied the page is not the screen's
        } else if (!R8(m + 0x30) && R16s(m + 0x22) == g_pcs.tap_layout->apply_item) {
            TapA();
            g_pcs.tap_apply = 0;
        }
    }
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
        // what asked for it, still held, stays hidden until let go; the tap that picks the line goes through, and B
        // calls it off and backs out as ever
        if (b_edge) {
            g_pcs.want_layout = nullptr;
            g_pc.selected = false;
        }
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

bool StartPrompt() {
    uint32_t mode = game::Opt().button_prompts;
    return mode == 1 || (mode != 2 && input::KeyboardActive());
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_00321160);
X86_EXTERN_FUNC(__imp__sub_00316DD0);

// Hide the HUD (the Gameplay menu): in a level with no menu up, the HUD's pictures and texts are not drawn. (The
// crosshair is left out in src/game/pc_settings.cpp; a mission's title card and the subtitles stay.)
bool HudHidden() {
    return game::Opt().hide_hud && !R32(kFrontEnd) && R32(kInGame) && Frame() - g_pointer_frame > 2 &&
           !(g_dialog_shown && Frame() - g_dialog_frame <= 2);
}

// a picture widget drawn (0x316DD0, vtable slot +0x44, ecx the widget): the sliders of the lines PC Options puts aside
// are not
X86_FUNC(sub_00316DD0) {
    PcScreenDrawMarks(ctx, ctx.ecx);
    if (PcScreenHidesPicture(ctx.ecx) || HudHidden() || TitleHidden()) {
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
X86_EXTERN_FUNC(__imp__sub_003145C0);
X86_EXTERN_FUNC(__imp__sub_00328E90);

// the main menu's tick (edi screen, the selected line on the stack): shows "check0<n+1>", the box printed beside line
// n. The lines below the hidden Multiplayer moved up one, so their tick goes one box up with them
X86_FUNC(sub_00328E90) {
    int32_t line = int32_t(R32(ctx.esp + 4));
    if (R32(ctx.edi) == kMainMenuVtable && g_main_gone >= 0 && line > g_main_gone) W32(ctx.esp + 4, uint32_t(line - 1));
    __imp__sub_00328E90(ctx, base);
}

// the title screen's draw (vtable 0x8297F0 slot +0x28, ecx the screen: its pictures, [+0x34], by 0x310010): not drawn,
// whether it is the front end's screen or on its way out behind the next one
X86_FUNC(sub_003145C0) {
    if (R32(ctx.ecx) == 0x8297F0 && !TitleWanted()) {
        ctx.esp += 4;  // its ret
        return;
    }
    __imp__sub_003145C0(ctx, base);
}

// a button prompt's draw (ecx prompt, vtable 0x82A450: a button picture and words, as "Press A to select"). The
// front end's top right one is Y and "Xbox Live" (made at 0x32A660, at 603,52 in 640x480): not drawn
X86_FUNC(sub_0030F900) {
    uint32_t prompt = ctx.ecx;
    if (g_foot_draw) {  // (a copy for the black edge, or part of a moved one)
        __imp__sub_0030F900(ctx, base);
        return;
    }
    bool live_corner = R32(prompt) == 0x82A450 && RF(prompt + 0x2C) > 560.0f && RF(prompt + 0x30) < 80.0f;
    if (live_corner || TitleHidden()) {
        if (live_corner) g_live_frame = Frame();
        ctx.esp += 4;  // the draw's own ret, skipped with it
        return;
    }
    if (R32(prompt) == 0x82A450) {
        std::lock_guard lock(g_drawn_lock);
        if (g_prompts.size() > 64) g_prompts.clear();
        g_prompts[prompt] = Frame();
    }
    // (a mission's "Press A to start the game" is put in the string table: src/game/pc_texts.cpp)
    Foot foot = R32(prompt) == 0x82A450 ? FootBegin(ctx, prompt, 0x30F900) : Foot{};
    __imp__sub_0030F900(ctx, base);
    FootEnd(prompt, foot);
}

// a text widget's draw (ecx widget)
X86_FUNC(sub_00321160) {
    uint32_t widget = ctx.ecx;
    if (g_foot_draw) {  // (a copy for the black edge, or part of a moved one)
        __imp__sub_00321160(ctx, base);
        return;
    }
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
    // (nor "Press START" on the title screen's picture)
    bool press_start = drawn_text && R32(kFrontEnd) && SameText(drawn_text, "Press START");
    if (live_corner || press_start || TitleHidden() || Hidden(widget) || (drawn_text && XboxLiveText(drawn_text)) || HudHidden()) {
        ctx.esp += 4;  // the draw's own ret, skipped with it
        return;
    }
    PcScreenText(widget);
    PcScreenDrawPager(ctx, widget);
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
    Foot foot = FootBegin(ctx, widget, 0x321160);
    __imp__sub_00321160(ctx, base);
    FootEnd(widget, foot);
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
