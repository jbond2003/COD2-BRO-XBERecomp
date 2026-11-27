// Water (Graphics; the game's own unless chosen). Reflections: the sea, a river or a pond mirrors what stands over
// it. New: the port's own water is drawn in its place.
//
// The game draws a body of water as one level sheet in the world scene's solid list: a few vertices, no blend, depth
// written, its own pixel shader over an animated picture on stages 0 and 1 (w_l1_0000.dds and on, wave0000.dds and
// on; water_l2_<mission>.dds where the water stands still) and highlights on stage 2 (water_l3_<mission>.dds).
// Nothing of that is changed. The renderer draws the sheet a second time over itself (d3d::Reflections) with what the
// last frame's picture held along each pixel's mirrored line of sight: the sky, the far shore, a ship. It knows the
// sheet by the picture on stage 0, which this file marks (gpu::texture_override's marker): the pictures are files of
// the missions' area packs (D:\sp\<mission>\...\<area>.cod), 128 texels square, and a texture in memory holds the
// file's bytes past its header. The surf's foam strips (water_l3_omaha on stage 0, alpha tested) and the splashes
// (water_wave) are other pictures and stay as they are.
//
// New water is that second draw covering the first: waves that move, the water's own colour darker where it is looked
// down into, the mirror, the sky's colour where the picture has nothing to mirror, and the sun's glint. The renderer
// is told the colours, which are the mission's: the water's is what the game's picture of it averages (the picture
// last put in a texture), the sky's what the mission's dome averages (src/game/sky.cpp), the sun the one the shadows
// fall away from (src/game/shadows.cpp). Far off and in the fog the game's own sheet shows.
//
// The renderer needs the frame's view before the water is drawn: the hook is in nglRenderSceneTree (0x74330) ahead of
// the scene's solid list (the call at 0x743F6; src/game/ambient_occlusion.cpp's comes after it). A scene: +0x10 its
// camera to screen matrix, +0x150 its world to camera matrix (row vectors, the world's up its third row), +0x300 1
// for perspective, +0x30C the solid nodes, +0x3C8 how far it sees.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "gpu/texture.h"
#include "gpu/texture_override.h"
#include "kernel/disc.h"
#include "kernel/kernel.h"
#include "xdk/d3d8.h"

namespace game {

std::vector<std::pair<std::string, std::vector<uint8_t>>> ReadPackFiles(const char* pack,
                                                                        const std::vector<std::string>& starts);
bool SkyTone(float tone[3]);     // sky.cpp: the colour of the sky last drawn
bool SunForWater(float way[3]);  // shadows.cpp: the way to the level's sun

namespace {

constexpr uint32_t kScene = 0x90259C;
constexpr uint32_t kFrontEnd = 0x95F8F8;
constexpr float kWorldFar = 500.0f;  // a scene that sees less far is not the world's
constexpr float kStrength = 1.0f;    // of the mirror the renderer works out
// new water: how rough each kind is drawn. The sea's frames (w_l1_) at full; the other moving water (wave00) a
// little less; water that stands still in the game (water_l2) calm.
constexpr float kRoughSea = 1.0f, kRoughMoving = 0.75f, kRoughStill = 0.45f;
constexpr float kGlint = 0.9f;       // the sun's glint: this much of the sky's brightness, at most kMostGlint
constexpr float kMostGlint = 0.6f;
constexpr float kEase = 2.0f;        // the colours go to another water's or sky's at this rate a second

struct Water {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> level0;
    float tone[3] = {};  // what the picture averages
    float rough = 1;
};

std::vector<Water> g_waters;          // the sheets' pictures, each once
std::atomic<bool> g_ready{false};     // g_waters is whole
std::atomic<int> g_drawn{-1};         // the one of them last put in a texture: the water of where the player is
std::once_flag g_read;

uint32_t U32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
}

bool Ends(const std::string& s, const char* end) {
    size_t n = std::strlen(end);
    return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

// what a picture's first level averages, from its blocks' two colours and how many texels take each
void Tone(const std::vector<uint8_t>& level0, bool dxt1, float tone[3]) {
    size_t size = dxt1 ? 8 : 16;
    double sum[3] = {};
    size_t blocks = 0;
    for (size_t at = 0; at + size <= level0.size(); at += size, blocks++) {
        const uint8_t* c = level0.data() + at + (dxt1 ? 0 : 8);
        uint16_t c0 = uint16_t(c[0] | c[1] << 8), c1 = uint16_t(c[2] | c[3] << 8);
        uint32_t bits = uint32_t(c[4]) | uint32_t(c[5]) << 8 | uint32_t(c[6]) << 16 | uint32_t(c[7]) << 24;
        bool four = !dxt1 || c0 > c1;
        // each texel's share of the first colour (the rest is the second's; three colour blocks' fourth is black)
        static constexpr double kFour[4] = {1, 0, 2.0 / 3, 1.0 / 3}, kThree[4] = {1, 0, 0.5, 0};
        double first = 0, second = 0;
        for (int i = 0; i < 16; i++) {
            uint32_t pick = (bits >> (2 * i)) & 3;
            first += four ? kFour[pick] : kThree[pick];
            second += four ? 1 - kFour[pick] : (pick == 3 ? 0 : 1 - kThree[pick]);
        }
        const double a[3] = {double(c0 >> 11) / 31, double((c0 >> 5) & 63) / 63, double(c0 & 31) / 31};
        const double b[3] = {double(c1 >> 11) / 31, double((c1 >> 5) & 63) / 63, double(c1 & 31) / 31};
        for (int k = 0; k < 3; k++) sum[k] += (a[k] * first + b[k] * second) / 16;
    }
    for (int k = 0; k < 3; k++) tone[k] = blocks ? float(sum[k] / double(blocks)) : 0.3f;
}

void WalkDisc(const kernel::Disc::Entry& dir, const std::string& path, std::vector<std::string>& out) {
    for (const kernel::Disc::Entry& e : dir.children) {
        if (e.dir) {
            WalkDisc(e, path + "\\" + e.name, out);
        } else {
            std::string name = e.name;
            for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (Ends(name, ".cod")) out.push_back(path + "\\" + e.name);
        }
    }
}

// every pack under D:\sp, from the disc image or the folder
std::vector<std::string> Packs() {
    std::vector<std::string> out;
    const std::string root = "D:\\sp";
    if (const kernel::Disc* disc = kernel::MountedDisc()) {
        const kernel::Disc::Entry* sp = disc->Find(std::string_view("sp"));
        if (sp && sp->dir) WalkDisc(*sp, root, out);
        return out;
    }
    std::string host = kernel::ResolvePath(root, 0);
    if (host.empty()) return out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(host), ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code is;
        if (!it->is_regular_file(is)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".cod") continue;
        std::string rel = fs::relative(it->path(), fs::path(host), is).generic_string();
        if (is || rel.empty()) continue;
        for (char& c : rel)
            if (c == '/') c = '\\';
        out.push_back(root + "\\" + rel);
    }
    return out;
}

// off the render thread (some hundreds of packs' directories): then every texture is asked about again
void Read() {
    std::vector<Water> found;
    std::vector<std::string> packs = Packs();
    for (const std::string& pack : packs) {
        for (auto& [name, dds] : ReadPackFiles(pack.c_str(), {"w_l1_", "wave0", "water_l2"})) {
            if (!Ends(name, ".dds") || dds.size() <= 128 || std::memcmp(dds.data(), "DDS ", 4)) continue;
            Water w;
            w.height = U32(dds, 12);
            w.width = U32(dds, 16);
            const char* fourcc = reinterpret_cast<const char*>(dds.data() + 84);
            bool dxt1 = !std::memcmp(fourcc, "DXT1", 4);
            bool dxt35 = !std::memcmp(fourcc, "DXT3", 4) || !std::memcmp(fourcc, "DXT5", 4);
            if ((!dxt1 && !dxt35) || w.width < 16 || w.height < 16 || w.width > 1024 || w.height > 1024 ||
                (w.width & 3) || (w.height & 3))
                continue;
            size_t size = size_t(w.width / 4) * (w.height / 4) * (dxt1 ? 8 : 16);
            if (128 + size > dds.size()) continue;
            w.level0.assign(dds.begin() + 128, dds.begin() + 128 + size);
            Tone(w.level0, dxt1, w.tone);
            w.rough = !name.compare(0, 5, "w_l1_") ? kRoughSea : !name.compare(0, 5, "wave0") ? kRoughMoving : kRoughStill;
            bool have = false;
            for (const Water& o : found) have = have || (o.width == w.width && o.height == w.height && o.level0 == w.level0);
            if (!have) found.push_back(std::move(w));
        }
    }
    LOGR("reflections: %zu pictures of water found in %zu packs", found.size(), packs.size());
    g_waters = std::move(found);
    g_ready.store(true);
    if (!g_waters.empty()) gpu::texture_override::Changed();
}

// render thread: what the renderer is to know of a texture's bytes
uint8_t Mark(const gpu::TextureDesc& d, const uint8_t* guest) {
    std::call_once(g_read, [] { std::thread(Read).detach(); });
    if (!g_ready.load() || d.cube || d.depth > 1) return 0;
    for (size_t i = 0; i < g_waters.size(); i++) {
        const Water& w = g_waters[i];
        if (w.width == d.width && w.height == d.height && gpu::LevelSize(d, 0) == w.level0.size() &&
            !std::memcmp(guest, w.level0.data(), w.level0.size())) {
            g_drawn.store(int(i), std::memory_order_relaxed);
            return gpu::texture_override::kMirror;
        }
    }
    return 0;
}

// New water's colours for this frame, false while no water's picture is known. They ease to another water's or
// sky's (an area's pictures are put in textures as it comes near, not when its water is first seen).
bool NewWater(const float* fog, d3d::Water& out) {
    int drawn = g_drawn.load(std::memory_order_relaxed);
    if (!g_ready.load() || drawn < 0 || size_t(drawn) >= g_waters.size()) return false;
    const Water& w = g_waters[size_t(drawn)];
    float want[7] = {w.tone[0], w.tone[1], w.tone[2], w.rough};
    // the sky: the mission's dome; without one the scene's fog, which is made to meet it; else an overcast grey
    if (!SkyTone(want + 4)) {
        for (int k = 0; k < 3; k++) want[4 + k] = fog ? fog[k] : 0.6f;
    }
    static float now[7];
    static bool set = false;
    static auto at = std::chrono::steady_clock::now();
    auto time = std::chrono::steady_clock::now();
    float step = std::clamp(std::chrono::duration<float>(time - at).count() * kEase, 0.0f, 1.0f);
    at = time;
    for (int k = 0; k < 7; k++) now[k] = set ? now[k] + (want[k] - now[k]) * step : want[k];
    set = true;
    for (int k = 0; k < 3; k++) out.tone[k] = now[k], out.sky[k] = now[4 + k];
    out.rough = now[3];
    // the glint is the sky's light: dim in a mission at night
    float bright = 0.3f * now[4] + 0.59f * now[5] + 0.11f * now[6];
    out.glint = SunForWater(out.sun) ? std::min(bright * kGlint, kMostGlint) : 0.0f;
    if (!(out.glint > 0)) out.sun[0] = out.sun[1] = 0, out.sun[2] = 1;
    return true;
}

}  // namespace

void InitReflections() { gpu::texture_override::SetMarker(Mark); }

}  // namespace game

// nglRenderSceneTree, the scene's solid list about to be drawn
void WaterReflections(X86Context&, uint8_t*) {
    using namespace kernel;
    uint32_t water = game::Opt().water;
    if (!water || R32(game::kFrontEnd)) return;
    uint32_t scene = R32(game::kScene);
    X86HostFp host_fp;
    bool solid = scene && R32(scene + 0x300) == 1 && R32(scene + 0x30C);
    // the world's scene, the first of a run of them (as the ambient occlusion tells it)
    static bool sent = false;
    if (!solid || !(G<float>(scene + 0x3C8) > game::kWorldFar)) {
        sent = false;
        return;
    }
    if (sent) return;
    sent = true;
    // the ripples' clock: counted round 64 seconds, which each ripple's turns divide
    static const auto start = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // new water, once a picture of the mission's water is known; until then (and where none is) the mirror alone
    d3d::Water whole{};
    float fog[3];
    bool fogged = G<uint8_t>(scene + 0x384) != 0;
    for (int k = 0; k < 3; k++) fog[k] = std::clamp(G<float>(scene + 0x3A0 + uint32_t(k) * 4), 0.0f, 1.0f);
    bool is_new = water >= 2 && game::NewWater(fogged ? fog : nullptr, whole);
    d3d::Reflections(&G<float>(scene + 0x10), &G<float>(scene + 0x150), game::kStrength, float(std::fmod(seconds, 64.0)),
                     is_new ? &whole : nullptr);
}
