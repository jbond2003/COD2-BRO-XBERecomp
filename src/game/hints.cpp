// Tutorial hints on the keyboard and mouse. The training hints are built from tokens ("[USE_LEFT_STICK] to move
// around") whose texts are entries of the global string table (sp/global.cod, loaded with the front end): loaded,
// an entry is a pointer to its text, a pointer to a "[text]" copy, the text padded with zeros and the copy. The button tokens ("Press ~cross") already show keycaps
// (prompts.cpp); the stick ones are words, so while the keyboard and mouse are in use those entries' texts are
// rewritten in place to the keys (within the room each entry has), and put back when a controller is used. The prone
// hint ("press crouch again") follows the new controls: Z on the keys, B held with the Modern layout.

#include <cstring>
#include <string>
#include <vector>

#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "core/log.h"
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
std::string Prone() { return "Press " + Key("prone", "Z") + " to go prone"; }
std::string HoldProne() { return "Hold [%s] to go prone"; }  // B held, its picture as before

Hint g_hints[] = {
    {"Move the left thumbstick", Move},
    {"Use the left analog stick", Move},
    {"Move the right thumbstick", Look},
    {"Use the right analog stick", Look},
    {"the left thumbstick", MoveLower},
    {"the right thumbstick", [] { return std::string("the mouse"); }},
    {"Click and hold the left thumbstick", Sprint},
    {"Press [%s] to go prone", Prone, HoldProne},  // the original: the crouch button again
};

bool g_searched = false;
bool g_keyboard = false;
bool g_modern = false;
uint32_t g_version = ~0u;

// each hint's entry in the loaded string table: the header, the text, its "[text]" copy
void Find() {
    uint8_t* mem = static_cast<uint8_t*>(kernel::HostPtr(mem::kContiguousBase));
    size_t size = mem::kPhysicalSize;
    int found = 0;
    for (Hint& h : g_hints) {
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
    std::string t = text.size() < h.room ? text : text.substr(0, h.room - 1);
    uint8_t* p = static_cast<uint8_t*>(kernel::HostPtr(h.at));
    std::memset(p, 0, h.room);
    std::memcpy(p, t.data(), t.size());
}

}  // namespace

namespace game {

// each client frame
void UpdateHints() {
    uint32_t mode = Opt().button_prompts;
    bool keyboard = mode == 1 || (mode != 2 && input::KeyboardActive());
    uint32_t version = input::BindingsVersion();
    if (!g_searched) {
        // the table loads with the front end; the hints are only shown in a level (no front end manager)
        if (kernel::R32(0x95F8F8) || !kernel::R32(0x95F8FC)) return;
        g_searched = true;
        Find();
        g_keyboard = !keyboard;
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
}

}  // namespace game
