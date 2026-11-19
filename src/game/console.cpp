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

// cmdlist and cvarlist, with an optional word the names must hold
void List(bool variables, const std::string& holding) {
    std::vector<std::string> lines;
    if (variables) {
        EachVariable([&](uint32_t var) {
            std::string name = Text(R32(var));
            if (!name.empty() && Lower(name).find(holding) != std::string::npos)
                lines.push_back("    " + name + " \"" + Text(R32(var + 4)) + "\"");
        });
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

void Names() {
    std::vector<std::string> names = {"clear", "cmdlist", "cvarlist", "help"};
    EachVariable([&](uint32_t var) {
        if (std::string name = Text(R32(var)); !name.empty()) names.push_back(name);
    });
    EachCommand([&](const std::string& name) {
        if (!name.empty()) names.push_back(name);
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
    return var && ((R32(var + 0x10) & kCheatVariable) || name.rfind("g_", 0) == 0);
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
                       "    cheats password, typed here or in the F10 panel.");
        return;
    }
    if (first == "cmdlist" || first == "cvarlist") return List(first == "cvarlist", Lower(rest));
    if (!cheats::Unlocked() && cheats::TryUnlock(line.c_str())) {
        console::Print("    Cheats are on until the game is closed.");
        return;
    }
    // cg_fov is the port's Field of View setting (src/game/fov.cpp sets the variable from it every frame, and keeps
    // the gun's size): typed here it changes the setting, in the menus too, where the variable does not exist yet
    if (first == "cg_fov") {
        if (!rest.empty()) {
            char* end = nullptr;
            double typed = std::strtod(rest.c_str() + (rest[0] == '"'), &end);
            if (end == rest.c_str() + (rest[0] == '"')) {
                console::Print("    cg_fov takes a number");
                return;
            }
            uint32_t fov = ClampFov(uint32_t(std::clamp(typed, 0.0, 1000.0) + 0.5));
            Opt().fov = fov;
            app::Save();
            LOGR("console: field of view %u", fov);
            if (double(fov) != typed)
                console::Print("    The field of view goes from " + std::to_string(kMinFov) + " to " + std::to_string(kMaxFov));
        }
        console::Print("    cg_fov is \"" + std::to_string(Opt().fov) + "\", default \"" + std::to_string(kDefaultFov) +
                       "\" (the Field of View setting)");
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
        } else if (!command && !GameCommand(first)) {
            console::Print("    Unknown command \"" + line.substr(0, space) + "\"");
            return;
        } else if (!command && R32(kFrontEnd)) {
            console::Print("    That one is the game's own: it needs a mission running.");
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
    if (console::NamesWanted()) Names();
    if (g_listen > 0) g_listen--;
    if (g_set_in > 0 && --g_set_in == 0) {
        for (const std::string& name : g_set)
            if (uint32_t var = FindVariable(name)) ShowVariable(var);
        g_set.clear();
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
