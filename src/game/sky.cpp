// The sky drawn smooth.
//
// A mission's sky is a dome with a small picture stretched over all of it (sky_<mission>_bkgd.dds, 64 or 128 texels
// square, DXT1) and layers of cloud over that (sky_<mission>_cl_*.dds, 256 texels, DXT5). On a console's 480 lines a
// texel of them was a few pixels; at a PC's resolution each is a block of the screen, with DXT1's steps of colour in it.
// The pictures are in each mission's pack (D:\sp\<mission>\<mission>.cod). Each is given to the renderer (the texture
// override of src/game/prompts.cpp) enlarged four times through a smooth curve instead of the sampler's straight
// lines, in 8 bits a colour with a dither, so the blocks and the steps go. Nothing is added to the pictures.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "gpu/texture.h"
#include "gpu/texture_override.h"

namespace game {

std::vector<std::pair<std::string, std::vector<uint8_t>>> ReadPackFiles(const char* pack, const std::string& start);

namespace {

using gpu::texture_override::Picture;

constexpr const char* kMissions[] = {"prologue", "oran",  "tunisia", "kasspass", "kassrine", "liberatr", "gela",
                                     "piano",    "troina", "omaha",   "mons",     "eilndrf",  "buchholz", "dragonth"};
constexpr int kTimes = 4;        // how much larger a picture is made
constexpr uint32_t kMost = 1024;  // the longest side made

struct Sky {
    std::string name;
    uint32_t width = 0, height = 0;
    bool dxt5 = false;
    bool dome = false;  // the picture behind the clouds: its edges do not meet
    std::vector<uint8_t> level0;
    std::shared_ptr<const Picture> smooth;
};

std::vector<Sky> g_skies;
std::once_flag g_read;
std::mutex g_lock;

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

bool Ends(const std::string& s, const char* end) {
    size_t n = std::strlen(end);
    return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

void Read() {
    for (const char* mission : kMissions) {
        std::string pack = std::string("D:\\sp\\") + mission + "\\" + mission + ".cod";
        for (auto& [name, dds] : ReadPackFiles(pack.c_str(), "sky_")) {
            if (!Ends(name, ".dds") || Ends(name, "_glow.dds") || dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4))
                continue;
            Sky s;
            s.name = name;
            s.height = U32(dds, 12);
            s.width = U32(dds, 16);
            const char* fourcc = reinterpret_cast<const char*>(dds.data() + 84);
            bool dxt1 = !std::memcmp(fourcc, "DXT1", 4);
            s.dxt5 = !std::memcmp(fourcc, "DXT5", 4);
            if ((!dxt1 && !s.dxt5) || s.width < 16 || s.height < 16 || s.width > 512 || s.height > 512 ||
                (s.width & 3) || (s.height & 3))
                continue;
            size_t size = size_t(s.width / 4) * (s.height / 4) * (dxt1 ? 8 : 16);
            if (128 + size > dds.size()) continue;
            s.dome = Ends(name, "_bkgd.dds");
            s.level0.assign(dds.begin() + 128, dds.begin() + 128 + size);
            g_skies.push_back(std::move(s));
        }
    }
    LOGR("sky: %zu pictures of the missions' skies found to draw smooth", g_skies.size());
}

// straight RGBA floats, 0 to 1
std::vector<float> Decode(const Sky& s) {
    std::vector<float> out(size_t(s.width) * s.height * 4, 1.0f);
    const uint8_t* block = s.level0.data();
    for (uint32_t by = 0; by < s.height; by += 4) {
        for (uint32_t bx = 0; bx < s.width; bx += 4, block += s.dxt5 ? 16 : 8) {
            const uint8_t* colour = block + (s.dxt5 ? 8 : 0);
            uint16_t c0 = uint16_t(colour[0] | colour[1] << 8), c1 = uint16_t(colour[2] | colour[3] << 8);
            float pal[4][4];
            auto rgb = [](uint16_t c, float* o) {
                o[0] = float(c >> 11) / 31.0f;
                o[1] = float((c >> 5) & 63) / 63.0f;
                o[2] = float(c & 31) / 31.0f;
                o[3] = 1.0f;
            };
            rgb(c0, pal[0]);
            rgb(c1, pal[1]);
            bool four = s.dxt5 || c0 > c1;
            for (int k = 0; k < 3; k++) {
                pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
                pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0.0f;
            }
            pal[2][3] = pal[3][3] = 1.0f;  // (DXT1's one transparent colour is not used by a sky: drawn black, opaque)
            float alpha[8] = {1, 1, 1, 1, 1, 1, 1, 1};
            uint64_t alpha_bits = 0;
            if (s.dxt5) {
                int a0 = block[0], a1 = block[1];
                alpha[0] = float(a0) / 255.0f;
                alpha[1] = float(a1) / 255.0f;
                for (int k = 2; k < 8; k++) {
                    if (a0 > a1)
                        alpha[k] = float((8 - k) * a0 + (k - 1) * a1) / (7.0f * 255.0f);
                    else
                        alpha[k] = k < 6 ? float((6 - k) * a0 + (k - 1) * a1) / (5.0f * 255.0f) : (k == 6 ? 0.0f : 1.0f);
                }
                for (int k = 0; k < 6; k++) alpha_bits |= uint64_t(block[2 + k]) << (8 * k);
            }
            uint32_t bits = uint32_t(colour[4]) | uint32_t(colour[5]) << 8 | uint32_t(colour[6]) << 16 |
                            uint32_t(colour[7]) << 24;
            for (int i = 0; i < 16; i++) {
                float* o = &out[(size_t(by + i / 4) * s.width + bx + i % 4) * 4];
                const float* p = pal[(bits >> (2 * i)) & 3];
                o[0] = p[0];
                o[1] = p[1];
                o[2] = p[2];
                o[3] = s.dxt5 ? alpha[(alpha_bits >> (3 * i)) & 7] : 1.0f;
            }
        }
    }
    return out;
}

// the weight of a texel t texels away: the cubic B-spline for the dome (no texel of it is left to see), Mitchell's
// curve for the clouds (keeps their edges)
float Weight(float t, bool soft) {
    float b = soft ? 1.0f : 1.0f / 3, c = soft ? 0.0f : 1.0f / 3;
    t = std::fabs(t);
    if (t < 1) return ((12 - 9 * b - 6 * c) * t * t * t + (-18 + 12 * b + 6 * c) * t * t + (6 - 2 * b)) / 6;
    if (t < 2) return ((-b - 6 * c) * t * t * t + (6 * b + 30 * c) * t * t + (-12 * b - 48 * c) * t + (8 * b + 24 * c)) / 6;
    return 0;
}

std::shared_ptr<const Picture> Enlarge(const Sky& s) {
    int times = kTimes;
    while (times > 1 && std::max(s.width, s.height) * uint32_t(times) > kMost) times /= 2;
    std::vector<float> in = Decode(s);
    for (size_t i = 0; i < in.size(); i += 4)  // colour weighted by alpha, so a clear texel's colour does not bleed
        for (int k = 0; k < 3; k++) in[i + k] *= in[i + 3];
    int w = int(s.width), h = int(s.height), ow = w * times, oh = h * times;
    // the clouds repeat across the sky: sideways always, up and down where the picture is square
    bool wrap_x = !s.dome, wrap_y = !s.dome && s.width == s.height;
    auto index = [](int i, int n, bool wrap) { return wrap ? ((i % n) + n) % n : std::clamp(i, 0, n - 1); };
    std::vector<float> wide(size_t(ow) * h * 4);
    for (int x = 0; x < ow; x++) {
        float at = (float(x) + 0.5f) / float(times) - 0.5f;
        int first = int(std::floor(at)) - 1;
        float wt[4];
        for (int k = 0; k < 4; k++) wt[k] = Weight(at - float(first + k), s.dome);
        for (int y = 0; y < h; y++) {
            float* o = &wide[(size_t(y) * ow + x) * 4];
            for (int k = 0; k < 4; k++) {
                const float* p = &in[(size_t(y) * w + index(first + k, w, wrap_x)) * 4];
                for (int ch = 0; ch < 4; ch++) o[ch] += wt[k] * p[ch];
            }
        }
    }
    auto pic = std::make_shared<Picture>();
    pic->width = uint32_t(ow);
    pic->height = uint32_t(oh);
    pic->argb.resize(size_t(ow) * oh);
    static const float kDither[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int y = 0; y < oh; y++) {
        float at = (float(y) + 0.5f) / float(times) - 0.5f;
        int first = int(std::floor(at)) - 1;
        float wt[4];
        const float* rows[4];
        for (int k = 0; k < 4; k++) {
            wt[k] = Weight(at - float(first + k), s.dome);
            rows[k] = &wide[size_t(index(first + k, h, wrap_y)) * ow * 4];
        }
        for (int x = 0; x < ow; x++) {
            float v[4] = {};
            for (int k = 0; k < 4; k++)
                for (int ch = 0; ch < 4; ch++) v[ch] += wt[k] * rows[k][size_t(x) * 4 + ch];
            float a = std::clamp(v[3], 0.0f, 1.0f);
            float d = (kDither[y & 3][x & 3] + 0.5f) / 16.0f;
            auto byte = [&](float f) { return uint32_t(std::clamp(f * 255.0f + d, 0.0f, 255.0f)); };
            uint32_t r = 255, g = 255, b = 255;
            if (a > 1.0f / 512) {
                r = byte(std::clamp(v[0] / a, 0.0f, 1.0f));
                g = byte(std::clamp(v[1] / a, 0.0f, 1.0f));
                b = byte(std::clamp(v[2] / a, 0.0f, 1.0f));
            }
            pic->argb[size_t(y) * ow + x] = byte(a) << 24 | r << 16 | g << 8 | b;
        }
    }
    return pic;
}

}  // namespace

// render thread: the smooth picture for one of the skies' textures, null for any other
std::shared_ptr<const Picture> SmoothSky(const gpu::TextureDesc& d, const uint8_t* guest) {
    if (d.cube || d.depth > 1 || d.width < 16 || d.height < 16 || d.width > 512 || d.height > 512) return nullptr;
    std::call_once(g_read, Read);
    for (Sky& s : g_skies) {
        if (s.width != d.width || s.height != d.height || gpu::LevelSize(d, 0) != s.level0.size() ||
            std::memcmp(guest, s.level0.data(), s.level0.size()) != 0)
            continue;
        std::lock_guard lock(g_lock);
        if (!s.smooth) {
            s.smooth = Enlarge(s);
            LOGR("sky: %s drawn smooth, %ux%u from %ux%u", s.name.c_str(), s.smooth->width, s.smooth->height, s.width,
                 s.height);
        }
        return s.smooth;
    }
    return nullptr;
}

}  // namespace game
