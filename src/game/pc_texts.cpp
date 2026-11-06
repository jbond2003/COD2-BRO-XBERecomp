// Texts of the game that speak of the console, said as on a PC. They are entries of the global string table
// (sp/global.cod; loaded, an entry is a pointer to its text, a pointer to a "[text]" copy, then the text: see
// hints.cpp), and a screen showing one holds its own copy as a game string (u16 capacity, u16 length, u16 references,
// the text). Both are rewritten in place wherever they are found, the new text being no longer than the old.

#include <algorithm>
#include <cstring>
#include <functional>
#include <string_view>

#include "core/log.h"
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

uint32_t Rewrite(const Text& t) {
    uint8_t* mem = static_cast<uint8_t*>(kernel::HostPtr(mem::kContiguousBase));
    size_t size = mem::kPhysicalSize;
    size_t old_len = std::strlen(t.xbox), new_len = std::strlen(t.pc);
    std::string_view want(t.xbox, old_len + 1);  // with its zero
    std::boyer_moore_horspool_searcher search(want.begin(), want.end());
    uint32_t done = 0;
    for (uint8_t* at = mem; at < mem + size;) {
        auto [found, end] = search(reinterpret_cast<const char*>(at), reinterpret_cast<const char*>(mem + size));
        if (found == reinterpret_cast<const char*>(mem + size)) break;
        uint8_t* p = reinterpret_cast<uint8_t*>(const_cast<char*>(found));
        size_t off = size_t(p - mem);
        at = p + want.size();
        // a game string's copy: its length before the text
        if (off >= 4) {
            uint16_t len;
            std::memcpy(&len, p - 4, 2);
            if (len == old_len) {
                uint16_t now = uint16_t(new_len);
                std::memcpy(p - 4, &now, 2);
            }
        }
        std::memset(p, 0, old_len);
        std::memcpy(p, t.pc, new_len);
        done = mem::kContiguousBase + uint32_t(off);
    }
    return done;
}

}  // namespace

namespace game {

// each client frame. The table loads once, with the front end: it is searched for (a whole-memory search, so only
// every half second, never in a level) until one of the texts is found; then only the found entries are looked at,
// and all are searched for again if one of them goes (the table loaded again elsewhere)
void UpdatePcTexts() {
    static uint32_t frames = 0;
    static uint32_t found[std::size(kTexts)] = {};
    static bool searching = true;
    frames++;
    for (size_t i = 0; i < std::size(kTexts) && !searching; i++) {
        if (!found[i]) continue;
        const Text& t = kTexts[i];
        const char* at = static_cast<const char*>(kernel::HostPtr(found[i]));
        if (std::strcmp(at, t.pc) == 0) continue;
        if (std::strcmp(at, t.xbox) == 0) {
            char* w = static_cast<char*>(kernel::HostPtr(found[i]));  // written again: put right in place
            std::memset(w, 0, std::strlen(t.xbox));
            std::memcpy(w, t.pc, std::strlen(t.pc));
            continue;
        }
        searching = true;
    }
    if (!searching) return;
    bool level = !kernel::R32(0x95F8F8) && kernel::R32(0x95F8FC);
    if (frames % 15 || level) return;
    int n = 0;
    for (size_t i = 0; i < std::size(kTexts); i++)
        if ((found[i] = Rewrite(kTexts[i]))) n++;
    if (!n) return;
    searching = false;
    LOGR("PC texts: %d of %zu", n, std::size(kTexts));
}

}  // namespace game
