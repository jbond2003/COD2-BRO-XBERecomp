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
// every half second) until found, then only its entry is looked at
void UpdatePcTexts() {
    static uint32_t frames = 0;
    static uint32_t found[std::size(kTexts)] = {};
    frames++;
    bool level = !kernel::R32(0x95F8F8) && kernel::R32(0x95F8FC);  // in a level: never a search
    for (size_t i = 0; i < std::size(kTexts); i++) {
        const Text& t = kTexts[i];
        if (found[i]) {
            const char* at = static_cast<const char*>(kernel::HostPtr(found[i]));
            if (std::strcmp(at, t.pc) == 0) continue;
            if (std::strcmp(at, t.xbox) != 0) {
                found[i] = 0;  // the table went (loaded again elsewhere): searched for again
                continue;
            }
        }
        if (!found[i] && (frames % 15 || level)) continue;
        uint32_t at = Rewrite(t);
        if (at && !found[i]) LOGR("PC text: \"%s\"", t.pc);
        found[i] = at;
    }
}

}  // namespace game
