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

int Rewrite(const Text& t) {
    uint8_t* mem = static_cast<uint8_t*>(kernel::HostPtr(mem::kContiguousBase));
    size_t size = mem::kPhysicalSize;
    size_t old_len = std::strlen(t.xbox), new_len = std::strlen(t.pc);
    std::string_view want(t.xbox, old_len + 1);  // with its zero
    std::boyer_moore_horspool_searcher search(want.begin(), want.end());
    int done = 0;
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
        done++;
    }
    return done;
}

}  // namespace

namespace game {

// each client frame: now and then while not in a level (the table loads with the front end, a screen copies its text)
void UpdatePcTexts() {
    static uint32_t frames = 0;
    if (frames++ % 15) return;
    if (!kernel::R32(0x95F8F8) && kernel::R32(0x95F8FC)) return;  // a level
    for (const Text& t : kTexts) {
        int n = Rewrite(t);
        if (n) LOGR("PC text: \"%s\" (%d copies)", t.pc, n);
    }
}

}  // namespace game
