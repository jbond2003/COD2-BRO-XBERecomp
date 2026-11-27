// Water Reflections (Graphics, off unless chosen): the sea, a river or a pond mirrors what stands over it.
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
// The renderer needs the frame's view before the water is drawn: the hook is in nglRenderSceneTree (0x74330) ahead of
// the scene's solid list (the call at 0x743F6; src/game/ambient_occlusion.cpp's comes after it). A scene: +0x10 its
// camera to screen matrix, +0x150 its world to camera matrix (row vectors, the world's up its third row), +0x300 1
// for perspective, +0x30C the solid nodes, +0x3C8 how far it sees.

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

namespace {

constexpr uint32_t kScene = 0x90259C;
constexpr uint32_t kFrontEnd = 0x95F8F8;
constexpr float kWorldFar = 500.0f;  // a scene that sees less far is not the world's
constexpr float kStrength = 1.0f;    // of the mirror the renderer works out

struct Water {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> level0;
};

std::vector<Water> g_waters;          // the sheets' pictures, each once
std::atomic<bool> g_ready{false};     // g_waters is whole
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
    for (const Water& w : g_waters) {
        if (w.width == d.width && w.height == d.height && gpu::LevelSize(d, 0) == w.level0.size() &&
            !std::memcmp(guest, w.level0.data(), w.level0.size()))
            return gpu::texture_override::kMirror;
    }
    return 0;
}

}  // namespace

void InitReflections() { gpu::texture_override::SetMarker(Mark); }

}  // namespace game

// nglRenderSceneTree, the scene's solid list about to be drawn
void WaterReflections(X86Context&, uint8_t*) {
    using namespace kernel;
    if (!game::Opt().water_reflections || R32(game::kFrontEnd)) return;
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
    d3d::Reflections(&G<float>(scene + 0x10), &G<float>(scene + 0x150), game::kStrength, float(std::fmod(seconds, 64.0)));
}
