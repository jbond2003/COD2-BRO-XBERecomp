// The console's lines on the game's thread (common/console.h has the screen). The engine keeps Quake III's command
// buffer: a line goes to Cbuf_AddText (0x2AABB0, text in eax) and Cmd_ExecuteString (0x2C6140) runs it next frame as
// a registered command (the list at [0x929B04]: {next, name, function}), else a variable ("name value" sets it
// through Cvar_Set2 0x29C030; the variables hang off the 256 hash chains at 0x9472F8: name +0, string +4, default
// +8, flags +0x10, next in chain +0x28), else it is sent to the game as a client command (ClientCommand 0x2507D0:
// the tables {name, function} at 0x8DDAE0, 18 entries, and 0x8DDB70, 16: god, noclip, give ...).
//
// What this Xbox build lost is the answers: Com_Printf is gone with every call to it, so a variable typed alone
// printed nothing, nor did a mistyped command. Those are answered here: a variable's value, a variable's value after
// it is set (so a write protected or cheat protected one shows that it did not change), "unknown command", and
// cmdlist / cvarlist. The few print functions that still format their text (the game's 0x248AF0, the client game's
// 0x277660, the renderer's 0x2AB760, each ending in a bare vsprintf) are shown for a moment after a typed line:
// "godmode ON", where's position.
//
// Cheats stay behind the password (src/game/cheats.cpp), which can be typed here too. A mission runs with sv_cheats
// on whether or not it was typed, so the engine itself would let god through: until the password is in, the console
// refuses the game's cheat commands, devmap, and writes to cheat protected variables (flag 0x200) and to the game
// rules (g_*).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "app/app.h"
#include "common/cheats.h"
#include "common/console.h"
#include "common/options.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

using kernel::R32;

namespace game {

namespace {

namespace console = app::console;

constexpr uint32_t kCbufAddText = 0x2AABB0, kCommands = 0x929B04, kVarChains = 0x9472F8, kFrontEnd = 0x95F8F8;
constexpr uint32_t kClientCommands = 0x8DDAE0, kClientCommandCount = 18;
constexpr uint32_t kPlayerCommands = 0x8DDB70, kPlayerCommandCount = 16;
constexpr int kListenFrames = 20;                       // how long after a typed line the engine's prints are shown

int g_listen = 0;                 // frames left
std::vector<std::string> g_set;   // variables a line just set: their values are shown once it has run
int g_set_in = 0;                 // ... in this many frames
uint32_t g_text = 0;              // a guest buffer for the line

// a guest pointer that can be followed
bool Pointer(uint32_t p) { return p >= 0x10000 && mem::IsCommitted(p); }

std::string Text(uint32_t p, size_t most = 1024, bool stack = false) {
    if (!stack && !Pointer(p)) return {};
    const char* s = static_cast<const char*>(kernel::HostPtr(p));
    return std::string(s, strnlen(s, most));
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

template <typename F>
void EachVariable(F f) {
    for (uint32_t chain = 0; chain < 256; chain++) {
        uint32_t var = R32(kVarChains + chain * 4);
        for (int n = 0; Pointer(var) && n < 4096; n++, var = R32(var + 0x28)) f(var);
    }
}

template <typename F>
void EachCommand(F f) {
    uint32_t node = R32(kCommands);
    for (int n = 0; Pointer(node) && n < 4096; n++, node = R32(node)) f(Text(R32(node + 4)));
    for (uint32_t i = 0; i < kClientCommandCount; i++) f(Text(R32(kClientCommands + i * 8)));
    for (uint32_t i = 0; i < kPlayerCommandCount; i++) f(Text(R32(kPlayerCommands + i * 8)));
}

uint32_t FindVariable(const std::string& lower) {
    uint32_t found = 0;
    EachVariable([&](uint32_t var) {
        if (!found && Lower(Text(R32(var))) == lower) found = var;
    });
    return found;
}

bool RegisteredCommand(const std::string& lower) {
    uint32_t node = R32(kCommands);
    for (int n = 0; Pointer(node) && n < 4096; n++, node = R32(node))
        if (Lower(Text(R32(node + 4))) == lower) return true;
    return false;
}

bool GameCommand(const std::string& lower) {
    for (uint32_t i = 0; i < kClientCommandCount; i++)
        if (Lower(Text(R32(kClientCommands + i * 8))) == lower) return true;
    for (uint32_t i = 0; i < kPlayerCommandCount; i++)
        if (Lower(Text(R32(kPlayerCommands + i * 8))) == lower) return true;
    return false;
}

void ShowVariable(uint32_t var) {
    console::Print("    " + Text(R32(var)) + " is \"" + Text(R32(var + 4)) + "\", default \"" + Text(R32(var + 8)) + "\"");
}

// The port's own settings as console variables: the ones the F10 panel and PC Options have. Each is the setting
// itself, read where it always was and saved in the settings file. Where the PC games have a name for the thing it
// is that name (cg_fov, cg_drawFPS, com_maxfps, r_gamma, sensitivity ...); the rest are the port's own, with the engine's prefix for their kind
// (cg_ what the player sees, r_ the renderer, m_ the mouse). cg_fov and
// com_maxfps are the engine's too, but the port's settings rule those here (src/game/fov.cpp sets cg_fov from the
// setting every frame; the frame rate is the window's limiter). The display mode and the resolution are not here:
// they change the window, on the panel's Apply.
struct PortVariable {
    const char* name;
    const char* what;   // the setting's name in the menus
    const char* takes;  // the values it takes, in words
    double low, high, standard;
    double step;        // values are rounded to a multiple of this
    double (*get)();
    void (*set)(double);
};

constexpr double kFilters[] = {0, 2, 4, 8, 16};

double Nearest(const double* values, size_t count, double v) {
    double best = values[0];
    for (size_t i = 1; i < count; i++)
        if (std::abs(values[i] - v) < std::abs(best - v)) best = values[i];
    return best;
}

#define PORT_BOOL(field) [] { return field ? 1.0 : 0.0; }, [](double v) { field = v != 0; }
#define PORT_NUMBER(field, type) [] { return double(field); }, [](double v) { field = type(v); }
const PortVariable kPortVariables[] = {
    {"cg_fov", "Field of View", "60 to 110", kMinFov, kMaxFov, kDefaultFov, 1, PORT_NUMBER(Opt().fov, uint32_t)},
    {"cg_drawFPS", "FPS Counter", "0 or 1", 0, 1, 0, 1, PORT_BOOL(Opt().fps_counter)},
    // any whole rate from 5 to 999 (his ask), not only the menu's six; 0 is no limit
    {"com_maxfps", "Max Frame Rate", "5 to 999, or 0 for no limit", 0, 999, 60, 1,
     PORT_NUMBER(app::Get().fps_cap, uint32_t)},
    {"r_swapInterval", "VSync", "0 or 1", 0, 1, 1, 1, PORT_BOOL(app::Get().vsync)},
    {"r_gamma", "Brightness", "0.5 to 2, 1 the game's own", 0.5, 2, 1, 0.1,
     [] { return double(Opt().brightness) / 100.0; }, [](double v) { Opt().brightness = uint32_t(v * 100.0 + 0.5); }},
    {"r_anisotropy", "Anisotropic Filtering", "0 (as the game asks), 2, 4, 8 or 16", 0, 16, 0, 1,
     PORT_NUMBER(app::Get().aniso, uint32_t)},
    {"sensitivity", "Mouse Horizontal Sensitivity", "0.1 to 5", 0.1, 5, 1, 0.1,
     PORT_NUMBER(Opt().mouse_sensitivity, float)},
    {"m_sensitivityY", "Mouse Vertical Sensitivity", "0.1 to 5", 0.1, 5, 1, 0.1,
     PORT_NUMBER(Opt().mouse_vertical_sensitivity, float)},
    {"m_invertMouse", "Invert Vertical Look", "0 or 1", 0, 1, 0, 1, PORT_BOOL(Opt().invert_mouse)},
    {"m_invertMouseX", "Invert Horizontal Look", "0 or 1", 0, 1, 0, 1, PORT_BOOL(Opt().invert_mouse_x)},
    {"r_renderScale", "Render Resolution", "20 to 200, percent", 20, 200, 100, 5,
     PORT_NUMBER(app::Get().render_percent, uint32_t)},
    {"cg_viewDistance", "View Distance", "0 low (the game's own), 1 medium or 2 high", 0, 2, 0, 1,
     [] { return Opt().view_distance <= 1 ? 0.0 : Opt().view_distance <= 5 ? 1.0 : 2.0; },
     [](double v) { Opt().view_distance = v < 0.5 ? 1 : v < 1.5 ? 4 : 8; }},
    {"cg_bodiesStay", "Bodies Stay", "0 or 1", 0, 1, 0, 1, PORT_BOOL(Opt().keep_bodies)},
    {"cg_cutsceneBars", "Cutscene Black Bars", "0 or 1", 0, 1, 1, 1, PORT_BOOL(Opt().cutscene_bars)},
    {"cg_aimAssist", "Controller Aim Assist", "0 or 1", 0, 1, 1, 1, PORT_BOOL(Opt().aim_assist)},
    {"cg_hints", "Tutorial Hints", "0 or 1", 0, 1, 1, 1, PORT_BOOL(Opt().hints)},
    {"cg_drawHud", "Hide the HUD", "1 the HUD is drawn, 0 it is hidden", 0, 1, 1, 1,
     [] { return Opt().hide_hud ? 0.0 : 1.0; }, [](double v) { Opt().hide_hud = v == 0; }},
    {"cg_leanToggle", "Leaning", "1 toggle, 0 hold as the game had it", 0, 1, 0, 1, PORT_BOOL(Opt().lean_toggle)},
    {"cg_buttonPrompts", "Button Prompts", "0 automatic, 1 keyboard and mouse, 2 controller", 0, 2, 0, 1,
     PORT_NUMBER(Opt().button_prompts, uint32_t)},
    {"cg_qwertyNames", "Profile Name Keys", "1 QWERTY, 0 ABC as the game had them", 0, 1, 1, 1,
     PORT_BOOL(Opt().qwerty_typewriter)},
    {"cg_walkSpeed", "walk key speed", "10 to 100, percent of running", 10, 100, 50, 1,
     PORT_NUMBER(Opt().walk_speed, uint32_t)},
    {"r_customTextures", "Texture Replacement", "0 or 1", 0, 1, 1, 1, PORT_BOOL(app::Get().custom_textures)},
    {"r_dumpTextures", "Texture Dumping", "0 or 1", 0, 1, 0, 1, PORT_BOOL(app::Get().dump_textures)},
};
#undef PORT_BOOL
#undef PORT_NUMBER

const PortVariable* FindPortVariable(const std::string& lower) {
    for (const PortVariable& v : kPortVariables)
        if (Lower(v.name) == lower) return &v;
    return nullptr;
}

// a typed value as the setting takes it
double Fit(const PortVariable& v, double typed) {
    double value = std::clamp(typed, v.low, v.high);
    value = std::round(value / v.step) * v.step;
    if (std::strcmp(v.name, "com_maxfps") == 0 && value > 0 && value < 5) value = 5;
    if (std::strcmp(v.name, "r_anisotropy") == 0) value = Nearest(kFilters, std::size(kFilters), value);
    return std::clamp(value, v.low, v.high);
}

std::string Number(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", v);
    std::string out = text;
    while (out.back() == '0') out.pop_back();
    if (out.back() == '.') out.pop_back();
    return out;
}

void ShowPortVariable(const PortVariable& v) {
    console::Print(std::string("    ") + v.name + " is \"" + Number(v.get()) + "\", default \"" + Number(v.standard) +
                   "\" (the " + v.what + " setting: " + v.takes + ")");
}

// cmdlist and cvarlist, with an optional word the names must hold
void List(bool variables, const std::string& holding) {
    std::vector<std::string> lines;
    if (variables) {
        EachVariable([&](uint32_t var) {
            std::string name = Text(R32(var));
            if (!name.empty() && Lower(name).find(holding) != std::string::npos && !FindPortVariable(Lower(name)))
                lines.push_back("    " + name + " \"" + Text(R32(var + 4)) + "\"");
        });
        for (const PortVariable& v : kPortVariables)
            if (Lower(v.name).find(holding) != std::string::npos)
                lines.push_back(std::string("    ") + v.name + " \"" + Number(v.get()) + "\" (" + v.what + ")");
    } else {
        EachCommand([&](const std::string& name) {
            if (!name.empty() && Lower(name).find(holding) != std::string::npos) lines.push_back("    " + name);
        });
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    std::string all;
    for (const std::string& line : lines) all += line + "\n";
    console::Print(all + std::to_string(lines.size()) + (variables ? " variables" : " commands"));
}

// The variables a mission makes when it loads, from the two tables its register loops read: the client game's
// (CG_RegisterCvars 0x27BB10: 0x8D9D60, 137 entries of {handle, name, default, flags}) and the game's
// (G_RegisterCvars 0x2530A0: 0x8DA8E8, [0x8DAE40] entries of 0x18 bytes that start the same way). They are in the
// image, so the menus can name them before they exist.
struct MissionVariable {
    std::string name, standard;
    uint32_t flags = 0;
};

template <typename F>
void EachMissionVariable(F f) {
    constexpr uint32_t kClientTable = 0x8D9D60, kClientCount = 137, kGameTable = 0x8DA8E8, kGameCount = 0x8DAE40;
    for (uint32_t i = 0; i < kClientCount; i++) {
        uint32_t at = kClientTable + i * 0x10;
        f(MissionVariable{Text(R32(at + 4)), Text(R32(at + 8)), R32(at + 0xC)});
    }
    uint32_t count = std::min<uint32_t>(R32(kGameCount), 512);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = kGameTable + i * 0x18;
        f(MissionVariable{Text(R32(at + 4)), Text(R32(at + 8)), R32(at + 0xC)});
    }
}

bool FindMissionVariable(const std::string& lower, MissionVariable& found) {
    bool any = false;
    EachMissionVariable([&](const MissionVariable& v) {
        if (!any && !v.name.empty() && Lower(v.name) == lower) found = v, any = true;
    });
    return any;
}

// Lines typed in the menus that need a mission (its variables, the game's own commands) are held, and run once one
// is under way: nothing is made in the engine ahead of its time.
std::vector<std::string> g_held;
int g_mission_frames = 0;  // frames in a row with a mission up and the player in it

bool Hold(const std::string& line) {
    constexpr size_t kHeld = 16;
    if (g_held.size() >= kHeld) {
        console::Print("    Too many lines are waiting for a mission already.");
        return false;
    }
    g_held.push_back(line);
    console::Print("    Held: it runs when a mission starts.");
    return true;
}

bool PlayerInMission() {
    constexpr uint32_t kEntities = 0x97A968, kEntityCount = 0x540;
    if (R32(kFrontEnd)) return false;
    for (uint32_t i = 0; i < kEntityCount; i++) {
        uint32_t ent = R32(kEntities + i * 8);
        if (ent && R32(ent + 0x23C)) return true;
    }
    return false;
}

void Names() {
    std::vector<std::string> names = {"clear", "cmdlist", "cvarlist", "help"};
    for (const PortVariable& v : kPortVariables) names.push_back(v.name);
    EachVariable([&](uint32_t var) {
        if (std::string name = Text(R32(var)); !name.empty() && !FindPortVariable(Lower(name))) names.push_back(name);
    });
    EachCommand([&](const std::string& name) {
        if (!name.empty()) names.push_back(name);
    });
    EachMissionVariable([&](const MissionVariable& v) {
        if (!v.name.empty() && !FindVariable(Lower(v.name)) && !FindPortVariable(Lower(v.name))) names.push_back(v.name);
    });
    console::SetNames(std::move(names));
}

void Send(X86Context& ctx, const std::string& line) {
    constexpr uint32_t kBytes = 512;
    if (!g_text) g_text = mem::AllocSystem(kBytes);
    if (!g_text || line.size() + 2 > kBytes) return;
    std::memcpy(kernel::HostPtr(g_text), (line + "\n").c_str(), line.size() + 2);
    uint32_t eax = ctx.eax;
    ctx.eax = g_text;
    kernel::CallGuest(ctx, kCbufAddText, {});
    ctx.eax = eax;
    g_listen = kListenFrames;
}

// the password is not in and the command is a cheat: one of the game's player commands (all but the harmless
// three), devmap, or a write to a cheat protected variable, direct or through set, toggle and the like
bool Locked(const std::string& first, const std::string& rest) {
    if (cheats::Unlocked()) return false;
    constexpr uint32_t kCheatVariable = 0x200;
    for (uint32_t i = 0; i < kPlayerCommandCount; i++)
        if (Lower(Text(R32(kPlayerCommands + i * 8))) == first)
            return first != "where" && first != "kill" && first != "dropweapon";
    if (first == "devmap") return true;
    std::string name = first;
    for (const char* setter : {"set", "seta", "sets", "toggle", "reset", "setfromcvar"})
        if (first == setter) name = Lower(rest.substr(0, rest.find_first_of(" \t")));
    if (name == first && (rest.empty() || RegisteredCommand(first))) return false;
    // the game rules (g_speed, g_gravity ...) are not marked as cheats, and are ones
    uint32_t var = FindVariable(name);
    MissionVariable later;
    uint32_t flags = var ? R32(var + 0x10) : FindMissionVariable(name, later) ? later.flags : 0;
    return (var || !later.name.empty()) && ((flags & kCheatVariable) || name.rfind("g_", 0) == 0);
}

void Run(X86Context& ctx, std::string line);

// a typed line: its commands, apart at each ; outside quotes
void RunLine(X86Context& ctx, const std::string& line) {
    console::Print("]" + line);
    LOGR("console: %s", line.c_str());
    std::string part;
    bool quoted = false;
    for (char c : line + ";") {
        if (c == '"') quoted = !quoted;
        if (c != ';' || quoted) {
            part += c;
            continue;
        }
        size_t from = part.find_first_not_of(" \t"), to = part.find_last_not_of(" \t");
        if (from != std::string::npos) Run(ctx, part.substr(from, to - from + 1));
        part.clear();
    }
}

void Run(X86Context& ctx, std::string line) {
    if (!line.empty() && (line[0] == '/' || line[0] == '\\')) line.erase(0, 1);
    size_t space = line.find_first_of(" \t");
    std::string first = Lower(line.substr(0, space));
    std::string rest = space == std::string::npos ? std::string() : line.substr(line.find_first_not_of(" \t", space));
    if (first.empty()) return;
    if (first == "clear") return console::Clear();
    if (first == "help") {
        console::Print("    Lines go to the game's own command system. cmdlist and cvarlist show what it has;\n"
                       "    either takes a word to look for (cvarlist cg_). A variable's name alone shows its value.\n"
                       "    Tab completes a name, Up and Down bring back earlier lines, Page Up and Page Down scroll.\n"
                       "    Cheat commands and variables (god, noclip, notarget, ufo, give, timescale ...) need the\n"
                       "    cheats password, typed here or in the F10 panel.\n"
                       "    In the menus, a line that needs a mission is held and runs when one starts.\n"
                       "    The port's own settings are variables too: cg_fov, cg_drawFPS, cg_drawHud,\n"
                       "    cg_viewDistance, cg_bodiesStay, com_maxfps, r_gamma, sensitivity ... (cvarlist marks them).");
        return;
    }
    if (first == "cmdlist" || first == "cvarlist") return List(first == "cvarlist", Lower(rest));
    if (!cheats::Unlocked() && cheats::TryUnlock(line.c_str())) {
        console::Print("    Cheats are on until the game is closed.");
        return;
    }
    // set and toggle of one of the port's settings: the engine's set would make a variable of its own
    if (first == "set" || first == "seta" || first == "sets" || first == "toggle") {
        size_t gap = rest.find_first_of(" \t");
        if (const PortVariable* port = FindPortVariable(Lower(rest.substr(0, gap)))) {
            if (first == "toggle") return Run(ctx, std::string(port->name) + (port->get() != 0 ? " 0" : " 1"));
            return Run(ctx, rest);
        }
    }
    // one of the port's own settings
    if (const PortVariable* port = FindPortVariable(first)) {
        if (!rest.empty()) {
            const char* from = rest.c_str() + (rest[0] == '"');
            char* end = nullptr;
            double typed = std::strtod(from, &end);
            if (end == from) {
                console::Print(std::string("    ") + port->name + " takes a number");
                return;
            }
            double value = Fit(*port, typed);
            port->set(value);
            app::Save();
            LOGR("console: %s %s", port->name, Number(value).c_str());
            if (value != typed) console::Print(std::string("    ") + port->name + " takes " + port->takes);
        }
        ShowPortVariable(*port);
        return;
    }
    if (Locked(first, rest)) {
        console::Print("    Cheats are locked: type the cheats password here or in the F10 panel.");
        return;
    }
    // what the engine would have said about it
    {
        bool command = RegisteredCommand(first);
        uint32_t var = command ? 0 : FindVariable(first);
        if (var && rest.empty()) return ShowVariable(var);
        if (var) {
            g_set.push_back(first);
            g_set_in = 3;
        } else if (MissionVariable later; !command && !GameCommand(first) && FindMissionVariable(first, later)) {
            // one of a mission's variables, and no mission (in one it would have been found above)
            if (rest.empty())
                console::Print("    " + later.name + " is made when a mission loads, default \"" + later.standard + "\"");
            else Hold(line);
            return;
        } else if (!command && !GameCommand(first)) {
            console::Print("    Unknown command \"" + line.substr(0, space) + "\"");
            return;
        } else if (!command && !PlayerInMission()) {
            Hold(line);  // the game's own command: it needs the player
            return;
        }
    }
    Send(ctx, line);
}

// a line the engine printed while the console listens
void Heard(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    if (!text.empty()) console::Print("    " + text);
}

}  // namespace

// each client frame (RunCommands, src/game/pad_layout.cpp)
void RunConsole(X86Context& ctx) {
    // a profile is signed in: the profile in use (the block at 0x8DCE30, its name first) has a name. It has none from
    // the start until one is picked or made in the Profile Manager.
    constexpr uint32_t kProfileName = 0x8DCE30;
    console::SetAllowed(kernel::G<uint8_t>(kProfileName) != 0);
    if (console::NamesWanted()) Names();
    if (g_listen > 0) g_listen--;
    if (g_set_in > 0 && --g_set_in == 0) {
        for (const std::string& name : g_set)
            if (uint32_t var = FindVariable(name)) ShowVariable(var);
        g_set.clear();
    }
    // the held lines, a moment into a mission
    constexpr int kSettled = 120;
    g_mission_frames = !g_held.empty() && PlayerInMission() ? std::min(g_mission_frames + 1, kSettled) : 0;
    if (g_mission_frames == kSettled && !g_held.empty()) {
        std::vector<std::string> held;
        held.swap(g_held);
        console::Print("The lines held in the menus:");
        for (const std::string& waiting : held) RunLine(ctx, waiting);
    }
    std::string line;
    while (console::Take(line)) RunLine(ctx, line);
}

}  // namespace game

// the bare vsprintf of a print function has just filled its buffer, now at esp+0xC (three arguments are still on
// the stack): the game's 0x248B0D, the client game's 0x27767D, the renderer's 0x2AB784
void ConsolePrinted(X86Context& ctx, uint8_t*) {
    if (game::g_listen > 0) game::Heard(game::Text(ctx.esp + 0xC, 1024, true));
}
