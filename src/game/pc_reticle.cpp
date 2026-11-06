// The original Call of Duty's (PC, 2003) crosshair in place of Big Red One's. Both games build the crosshair from the
// same pieces, named alike: a centre (center_dot), the sides that spread (side_skinny) and the arc of some weapons
// (side_small_arc, the grenades'). Big Red One's are in each level's weapons.cod; the PC game's are gfx/reticle/*.tga in its
// Main/pak5.pk3. Nothing of either game is in this repository: the PC pictures are read from a copy in the game files
// (pc\reticle\*.tga beside sp\), else from the player's own PC install (Steam's, or the folder in COD_PC_DIR), when
// the game asks for the matching texture; without either the crosshair stays as it is.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "common/options.h"
#include "core/log.h"
#include "gpu/texture_override.h"
#include "kernel/kernel.h"

namespace game {
bool ReadPackFile(const char* pack, const std::string& name, std::vector<uint8_t>& out);  // src/game/prompts.cpp
}

namespace {

using gpu::texture_override::Picture;
namespace fs = std::filesystem;

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}
uint16_t U16(const std::vector<uint8_t>& d, size_t at) {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
}

// --- Inflate (RFC 1951), for the pk3's deflated entries

class Inflater {
public:
    Inflater(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool Run(std::vector<uint8_t>& out) {
        bool last = false;
        while (!last) {
            last = Bits(1);
            uint32_t type = Bits(2);
            if (bad_) return false;
            if (type == 0) {
                pos_ = (pos_ + 7) & ~size_t(7);
                size_t at = pos_ / 8;
                if (at + 4 > size_) return false;
                uint32_t len = uint32_t(data_[at] | data_[at + 1] << 8);
                at += 4;
                if (at + len > size_) return false;
                out.insert(out.end(), data_ + at, data_ + at + len);
                pos_ = (at + len) * 8;
            } else if (type == 1 || type == 2) {
                Huffman lit, dist;
                if (type == 1) {
                    std::array<uint8_t, 288> l{};
                    for (int i = 0; i < 288; i++) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                    std::array<uint8_t, 30> d{};
                    d.fill(5);
                    lit.Build(l.data(), 288);
                    dist.Build(d.data(), 30);
                } else if (!Dynamic(lit, dist)) {
                    return false;
                }
                if (!Block(lit, dist, out)) return false;
            } else {
                return false;
            }
        }
        return !bad_;
    }

private:
    struct Huffman {
        std::array<uint16_t, 16> counts{};
        std::vector<uint16_t> symbols;
        void Build(const uint8_t* lengths, int n) {
            counts.fill(0);
            for (int i = 0; i < n; i++) counts[lengths[i]]++;
            counts[0] = 0;
            std::array<uint16_t, 16> offs{};
            for (int i = 1; i < 16; i++) offs[i] = uint16_t(offs[i - 1] + counts[i - 1]);
            symbols.assign(size_t(n), 0);
            for (int i = 0; i < n; i++)
                if (lengths[i]) symbols[offs[lengths[i]]++] = uint16_t(i);
        }
    };

    uint32_t Bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) {
            size_t byte = pos_ / 8;
            if (byte >= size_) {
                bad_ = true;
                return 0;
            }
            v |= uint32_t((data_[byte] >> (pos_ % 8)) & 1) << i;
            pos_++;
        }
        return v;
    }

    int Decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= int(Bits(1));
            int count = h.counts[len];
            if (code - count < first) return h.symbols[size_t(index + (code - first))];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            if (bad_) return -1;
        }
        bad_ = true;
        return -1;
    }

    bool Dynamic(Huffman& lit, Huffman& dist) {
        int hlit = int(Bits(5)) + 257, hdist = int(Bits(5)) + 1, hclen = int(Bits(4)) + 4;
        static constexpr int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t cl[19] = {};
        for (int i = 0; i < hclen; i++) cl[kOrder[i]] = uint8_t(Bits(3));
        Huffman codes;
        codes.Build(cl, 19);
        uint8_t lengths[320] = {};
        int n = 0;
        while (n < hlit + hdist) {
            int sym = Decode(codes);
            if (sym < 0) return false;
            if (sym < 16) {
                lengths[n++] = uint8_t(sym);
                continue;
            }
            int repeat = 0;
            uint8_t value = 0;
            if (sym == 16) {
                if (n == 0) return false;
                value = lengths[n - 1];
                repeat = 3 + int(Bits(2));
            } else if (sym == 17) {
                repeat = 3 + int(Bits(3));
            } else {
                repeat = 11 + int(Bits(7));
            }
            if (n + repeat > hlit + hdist) return false;
            while (repeat--) lengths[n++] = value;
        }
        lit.Build(lengths, hlit);
        dist.Build(lengths + hlit, hdist);
        return !bad_;
    }

    bool Block(const Huffman& lit, const Huffman& dist, std::vector<uint8_t>& out) {
        static constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                   33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = Decode(lit);
            if (sym < 0) return false;
            if (sym < 256) {
                out.push_back(uint8_t(sym));
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                size_t len = kLenBase[sym] + Bits(kLenExtra[sym]);
                int d = Decode(dist);
                if (d < 0 || d >= 30) return false;
                size_t back = kDistBase[d] + Bits(kDistExtra[d]);
                if (back > out.size() || bad_) return false;
                for (size_t i = 0; i < len; i++) out.push_back(out[out.size() - back]);
            }
        }
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool bad_ = false;
};

// --- The PC game's install and its pk3 (zip) files

std::vector<fs::path> InstallCandidates() {
    std::vector<fs::path> out;
    if (const char* dir = std::getenv("COD_PC_DIR"); dir && *dir) out.emplace_back(dir);
    std::vector<fs::path> steams = {"C:\\Steam", "C:\\Program Files (x86)\\Steam", "C:\\Program Files\\Steam"};
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) ==
        ERROR_SUCCESS)
        steams.insert(steams.begin(), fs::path(buf));
#endif
    // every Steam library the client knows (libraryfolders.vdf: "path" lines)
    std::vector<fs::path> libraries;
    for (const fs::path& steam : steams) {
        libraries.push_back(steam);
        std::ifstream vdf(steam / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(vdf, line)) {
            size_t key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            size_t a = line.find('"', key + 6), b = a == std::string::npos ? a : line.find('"', a + 1);
            if (a == std::string::npos || b == std::string::npos) continue;
            std::string p = line.substr(a + 1, b - a - 1), clean;
            for (size_t i = 0; i < p.size(); i++) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') i++;
                clean += p[i];
            }
            libraries.emplace_back(clean);
        }
    }
    for (const fs::path& lib : libraries) out.push_back(lib / "steamapps" / "common" / "Call of Duty");
    return out;
}

// one entry of a zip, inflated
bool ReadZipEntry(const fs::path& zip, const std::string& name, std::vector<uint8_t>& out) {
    std::ifstream in(zip, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    uint64_t size = uint64_t(in.tellg());
    uint64_t tail_len = std::min<uint64_t>(size, 66000);
    std::vector<uint8_t> tail(tail_len);
    in.seekg(std::streamoff(size - tail_len));
    in.read(reinterpret_cast<char*>(tail.data()), std::streamsize(tail_len));
    size_t eocd = std::string::npos;
    for (size_t i = tail_len >= 22 ? tail_len - 22 : 0;; i--) {
        if (U32(tail, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) return false;
    uint32_t dir_size = U32(tail, eocd + 12), dir_at = U32(tail, eocd + 16);
    std::vector<uint8_t> dir(dir_size);
    in.seekg(dir_at);
    in.read(reinterpret_cast<char*>(dir.data()), dir_size);
    if (!in) return false;
    for (size_t at = 0; at + 46 <= dir.size() && U32(dir, at) == 0x02014b50;) {
        uint16_t method = U16(dir, at + 10), name_len = U16(dir, at + 28), extra = U16(dir, at + 30),
                 comment = U16(dir, at + 32);
        uint32_t packed = U32(dir, at + 20), unpacked = U32(dir, at + 24), local = U32(dir, at + 42);
        std::string entry(reinterpret_cast<const char*>(dir.data() + at + 46), name_len);
        at += 46 + size_t(name_len) + extra + comment;
        if (entry.size() != name.size() ||
            !std::equal(entry.begin(), entry.end(), name.begin(), [](char a, char b) {
                return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
            }))
            continue;
        std::vector<uint8_t> header(30);
        in.seekg(local);
        in.read(reinterpret_cast<char*>(header.data()), 30);
        if (!in || U32(header, 0) != 0x04034b50) return false;
        std::vector<uint8_t> data(packed);
        in.seekg(std::streamoff(local + 30 + U16(header, 26) + U16(header, 28)));
        in.read(reinterpret_cast<char*>(data.data()), packed);
        if (!in) return false;
        out.clear();
        if (method == 0) {
            out = std::move(data);
        } else if (method == 8) {
            out.reserve(unpacked);
            if (!Inflater(data.data(), data.size()).Run(out)) return false;
        } else {
            return false;
        }
        return out.size() == unpacked;
    }
    return false;
}

// --- TGA (uncompressed or run-length, 24 or 32 bit) to a picture, each texel n by n so it stays sharp when stretched

std::shared_ptr<Picture> Tga(const std::vector<uint8_t>& t, uint32_t n) {
    if (t.size() < 18) return nullptr;
    uint8_t id_len = t[0], type = t[2], bpp = t[16], desc = t[17];
    uint32_t w = U16(t, 12), h = U16(t, 14);
    if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || !w || !h || w > 512 || h > 512) return nullptr;
    size_t px = bpp / 8, at = 18 + size_t(id_len);
    std::vector<uint32_t> texels;
    texels.reserve(size_t(w) * h);
    auto read = [&](size_t i) {
        uint32_t b = t[i], g = t[i + 1], r = t[i + 2], a = px == 4 ? t[i + 3] : 255;
        return a << 24 | r << 16 | g << 8 | b;
    };
    while (texels.size() < size_t(w) * h) {
        if (type == 2) {
            if (at + px > t.size()) return nullptr;
            texels.push_back(read(at));
            at += px;
            continue;
        }
        if (at >= t.size()) return nullptr;
        uint8_t head = t[at++];
        size_t count = size_t(head & 0x7F) + 1;
        if (head & 0x80) {
            if (at + px > t.size()) return nullptr;
            uint32_t v = read(at);
            at += px;
            for (size_t i = 0; i < count; i++) texels.push_back(v);
        } else {
            for (size_t i = 0; i < count; i++) {
                if (at + px > t.size()) return nullptr;
                texels.push_back(read(at));
                at += px;
            }
        }
    }
    texels.resize(size_t(w) * h);
    bool top_down = (desc & 0x20) != 0;
    auto pic = std::make_shared<Picture>();
    pic->width = w * n;
    pic->height = h * n;
    pic->argb.resize(size_t(pic->width) * pic->height);
    for (uint32_t y = 0; y < pic->height; y++)
        for (uint32_t x = 0; x < pic->width; x++) {
            uint32_t sy = y / n, sx = x / n;
            uint32_t row = top_down ? sy : h - 1 - sy;
            uint32_t v = texels[size_t(row) * w + sx];
            if ((v >> 24) == 0) v = 0x00FFFFFF & (v | 0x00FFFFFF);  // clear texels white, so filtering keeps edges
            pic->argb[size_t(y) * pic->width + x] = v;
        }
    return pic;
}

struct Piece {
    const char* bro;   // in weapons.cod
    const char* pc;    // in the PC game's pk3s
    const char* copy;  // a copy in the game files (D:\pc\reticle), read first
    std::vector<uint8_t> level0;  // Big Red One's texture as the game loads it
    std::shared_ptr<const Picture> picture;
};

std::once_flag g_loaded;
std::vector<Piece> g_pieces;  // written once, under g_loaded

void Load() {
    std::vector<Piece> pieces = {
        {"center_dot.dds", "gfx/reticle/hud@center_dot.tga", "D:\\pc\\reticle\\center_dot.tga", {}, nullptr},
        {"side_skinny.dds", "gfx/reticle/side_skinny.tga", "D:\\pc\\reticle\\side_skinny.tga", {}, nullptr},
        {"side_small_arc.dds", "gfx/reticle/hud@side_small_arc.tga", "D:\\pc\\reticle\\side_small_arc.tga", {},
         nullptr}};
    fs::path main;
    for (const fs::path& dir : InstallCandidates()) {
        std::error_code ec;
        if (fs::exists(dir / "Main" / "pak5.pk3", ec)) {
            main = dir / "Main";
            break;
        }
    }
    // the pk3s in the order the PC game loads them: a later one replaces a file of an earlier one
    std::vector<fs::path> paks;
    std::error_code ec;
    if (!main.empty()) {
        for (const auto& e : fs::directory_iterator(main, ec)) {
            std::string lower = e.path().filename().string();
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".pk3") paks.push_back(e.path());
        }
    }
    std::sort(paks.begin(), paks.end());
    int found = 0;
    for (Piece& p : pieces) {
        std::vector<uint8_t> dds, tga;
        if (!game::ReadPackFile("D:\\sp\\prologue\\weapons.cod", p.bro, dds) || dds.size() <= 128) continue;
        p.level0.assign(dds.begin() + 128, dds.end());
        if (!kernel::ReadGameFile(p.copy, tga)) tga.clear();
        for (auto it = paks.rbegin(); it != paks.rend() && tga.empty(); ++it) ReadZipEntry(*it, p.pc, tga);
        if (tga.empty()) continue;
        p.picture = Tga(tga, 8);
        if (p.picture) found++;
    }
    g_pieces = std::move(pieces);
    LOGR("PC crosshair: %d of %zu pieces (game files, then %s)", found, g_pieces.size(),
         main.empty() ? "no PC install" : main.string().c_str());
}

}  // namespace

namespace game {

// render thread, from the texture provider (prompts.cpp): the PC piece for one of Big Red One's crosshair textures
std::shared_ptr<const Picture> PcReticle(const gpu::TextureDesc& d, const uint8_t* guest) {
    if (!Opt().pc_crosshair) return nullptr;  // PC Options > Crosshair: Big Red One's own
    std::call_once(g_loaded, Load);
    size_t size = gpu::LevelSize(d, 0);
    for (const Piece& p : g_pieces)
        if (p.picture && size == p.level0.size() && std::memcmp(guest, p.level0.data(), size) == 0) {
            static std::once_flag logged;
            std::call_once(logged, [] { LOGR("PC crosshair: in use"); });
            return p.picture;
        }
    return nullptr;
}

}  // namespace game
