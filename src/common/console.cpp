// The developer console's lines and its screen (console.h). The lines are shared by the threads; the screen is
// Windows only, drawn with the settings panel's ImGui.
#include "common/console.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>

namespace app::console {

namespace {

constexpr size_t kLines = 600, kQueued = 32;

std::mutex g_lock;
std::deque<std::string> g_lines;      // under g_lock, oldest first
std::deque<std::string> g_submitted;  // under g_lock
std::vector<std::string> g_names;     // under g_lock, sorted
std::atomic<uint32_t> g_serial{0};    // moves when g_lines does
std::atomic<bool> g_open{false};
std::atomic<bool> g_names_wanted{false};

}  // namespace

bool Open() { return g_open.load(std::memory_order_relaxed); }

void Toggle() {
    bool open = !Open();
    g_open.store(open, std::memory_order_relaxed);
    if (open) g_names_wanted.store(true, std::memory_order_relaxed);
}

void Print(const std::string& text) {
    std::lock_guard lock(g_lock);
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at || end < text.size()) {
            std::string line = text.substr(at, end - at);
            // the engine's colour codes (^1 ... ^9) are not drawn
            for (size_t i = 0; i + 1 < line.size();)
                if (line[i] == '^' && std::isdigit(static_cast<unsigned char>(line[i + 1]))) line.erase(i, 2);
                else i++;
            for (char& c : line)
                if (static_cast<unsigned char>(c) < 0x20) c = ' ';
            g_lines.push_back(std::move(line));
        }
        at = end + 1;
    }
    while (g_lines.size() > kLines) g_lines.pop_front();
    g_serial.fetch_add(1, std::memory_order_relaxed);
}

void Clear() {
    std::lock_guard lock(g_lock);
    g_lines.clear();
    g_serial.fetch_add(1, std::memory_order_relaxed);
}

void Submit(const std::string& line) {
    std::lock_guard lock(g_lock);
    if (g_submitted.size() < kQueued) g_submitted.push_back(line);
}

bool Take(std::string& line) {
    std::lock_guard lock(g_lock);
    if (g_submitted.empty()) return false;
    line = std::move(g_submitted.front());
    g_submitted.pop_front();
    return true;
}

bool NamesWanted() { return g_names_wanted.exchange(false, std::memory_order_relaxed); }

void SetNames(std::vector<std::string> names) {
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::lock_guard lock(g_lock);
    g_names = std::move(names);
}

}  // namespace app::console

#ifdef _WIN32
#include <imgui.h>

namespace app::console {

namespace {

constexpr size_t kHistory = 64;

struct Screen {
    char input[256] = "";
    std::vector<std::string> lines;  // a copy of g_lines, taken when they change
    uint32_t serial = ~0u;
    std::vector<std::string> history;  // oldest first
    int recalled = -1;                 // index into history, -1 the line being typed
    bool was_open = false;
    bool to_end = true;
    std::string suggested_for;           // the word the suggestions are for, lower case
    std::vector<std::string> suggested;  // those starting with it first
    size_t suggested_starting = 0, suggested_more = 0;
} g_screen;

bool SameStart(const std::string& name, const char* word, size_t length) {
    if (name.size() < length) return false;
    for (size_t i = 0; i < length; i++)
        if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(word[i])))
            return false;
    return true;
}

// Tab: the word the caret is in or after grows to the one name that starts with it, or as far as every such name
// agrees, and those names are listed
void Complete(ImGuiInputTextCallbackData* data) {
    int end = data->CursorPos, start = end;
    while (start > 0 && data->Buf[start - 1] != ' ' && data->Buf[start - 1] != ';') start--;
    if (start < end && (data->Buf[start] == '/' || data->Buf[start] == '\\')) start++;
    if (start == end) return;
    std::vector<std::string> found;
    {
        std::lock_guard lock(g_lock);
        for (const std::string& name : g_names)
            if (SameStart(name, data->Buf + start, size_t(end - start))) found.push_back(name);
    }
    if (found.empty()) return;
    std::string grown = found[0];
    for (const std::string& name : found) {
        size_t same = 0;
        while (same < grown.size() && same < name.size() &&
               std::tolower(static_cast<unsigned char>(grown[same])) ==
                   std::tolower(static_cast<unsigned char>(name[same])))
            same++;
        grown.resize(same);
    }
    if (found.size() == 1) grown += ' ';
    data->DeleteChars(start, end - start);
    data->InsertChars(start, grown.c_str());
    if (found.size() == 1) return;
    constexpr size_t kListed = 60;
    std::string list;
    for (size_t i = 0; i < found.size() && i < kListed; i++) list += "    " + found[i] + "\n";
    if (found.size() > kListed) list += "    ... and " + std::to_string(found.size() - kListed) + " more\n";
    Print(list);
    g_screen.to_end = true;
}

// Up and Down: the lines typed before
void Recall(ImGuiInputTextCallbackData* data) {
    Screen& s = g_screen;
    int count = int(s.history.size()), was = s.recalled;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (s.recalled == -1) s.recalled = count - 1;
        else if (s.recalled > 0) s.recalled--;
    } else if (data->EventKey == ImGuiKey_DownArrow && s.recalled != -1) {
        if (++s.recalled >= count) s.recalled = -1;
    }
    if (s.recalled == was) return;
    data->DeleteChars(0, data->BufTextLen);
    if (s.recalled >= 0) data->InsertChars(0, s.history[size_t(s.recalled)].c_str());
}

// The names like the word being typed, listed under the console as in the PC games' own: those that start with it
// first, then those that hold it. Tab completes from the first kind.
void Suggest(ImVec2 at, ImVec4 back, ImVec4 edge, ImVec4 text, ImVec4 same) {
    Screen& s = g_screen;
    constexpr size_t kShown = 12;
    size_t length = std::strlen(s.input), start = length;
    while (start > 0 && s.input[start - 1] != ' ' && s.input[start - 1] != ';') start--;
    if (start < length && (s.input[start] == '/' || s.input[start] == '\\')) start++;
    std::string word(s.input + start, length - start);
    for (char& c : word) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (word != s.suggested_for) {
        s.suggested_for = word;
        s.suggested.clear();
        s.suggested_more = 0;
        if (!word.empty()) {
            std::vector<std::string> holding;
            std::lock_guard lock(g_lock);
            for (const std::string& name : g_names) {
                std::string lower = name;
                for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
                size_t found = lower.find(word);
                if (found == std::string::npos || lower == word) continue;
                (found == 0 ? s.suggested : holding).push_back(name);
            }
            s.suggested_starting = s.suggested.size();
            s.suggested.insert(s.suggested.end(), holding.begin(), holding.end());
            if (s.suggested.size() > kShown) {
                s.suggested_more = s.suggested.size() - kShown;
                s.suggested.resize(kShown);
            }
        }
    }
    if (s.suggested.empty()) return;
    float em = ImGui::GetFontSize(), line = ImGui::GetTextLineHeightWithSpacing();
    std::string more = s.suggested_more ? "... and " + std::to_string(s.suggested_more) + " more" : std::string();
    float width = more.empty() ? 0.0f : ImGui::CalcTextSize(more.c_str()).x;
    for (const std::string& name : s.suggested) width = std::max(width, ImGui::CalcTextSize(name.c_str()).x);
    size_t rows = s.suggested.size() + (more.empty() ? 0 : 1);
    ImVec2 a(at.x - em * 0.5f, at.y), b(at.x + width + em * 0.5f, at.y + line * float(rows) + em * 0.4f);
    ImDrawList* list = ImGui::GetForegroundDrawList();
    list->AddRectFilled(a, b, ImGui::GetColorU32(back));
    list->AddRect(a, b, ImGui::GetColorU32(edge));
    float y = at.y + em * 0.2f;
    for (size_t i = 0; i < s.suggested.size(); i++, y += line) {
        const std::string& name = s.suggested[i];
        list->AddText(ImVec2(at.x, y), ImGui::GetColorU32(text), name.c_str());
        // the part typed, in the typed lines' colour, where the name starts with it
        if (i < s.suggested_starting && word.size() <= name.size())
            list->AddText(ImVec2(at.x, y), ImGui::GetColorU32(same), name.c_str(), name.c_str() + word.size());
    }
    if (!more.empty()) list->AddText(ImVec2(at.x, y), ImGui::GetColorU32(ImVec4(text.x, text.y, text.z, 0.6f)), more.c_str());
}

int InputEvent(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        // the console's own key types nothing (` ~ on a US keyboard, ` and the not sign on a British one)
        ImWchar c = data->EventChar;
        return c == '`' || c == '~' || c == 0xAC || c == 0xA7 || c == 0xB1;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) Complete(data);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) Recall(data);
    return 0;
}

}  // namespace

void Draw() {
    Screen& s = g_screen;
    bool open = Open();
    bool opened = open && !s.was_open;
    s.was_open = open;
    if (!open) return;
    if (opened) s.to_end = true;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    constexpr ImVec4 kBack{0.03f, 0.04f, 0.05f, 0.90f}, kEdge{0.66f, 0.48f, 0.24f, 0.85f};
    constexpr ImVec4 kText{0.92f, 0.89f, 0.82f, 1.0f}, kTyped{0.93f, 0.66f, 0.26f, 1.0f};
    constexpr ImVec4 kField{0.10f, 0.11f, 0.14f, 1.0f}, kGrab{0.30f, 0.20f, 0.09f, 1.0f};
    ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, float(int(vp->Size.y * 0.45f))), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBack);
    ImGui::PushStyleColor(ImGuiCol_Border, kEdge);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kField);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, kGrab);
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.8f, em * 0.5f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar;
    if (ImGui::Begin("##console", nullptr, kFlags)) {
        if (uint32_t serial = g_serial.load(std::memory_order_relaxed); serial != s.serial) {
            std::lock_guard lock(g_lock);
            s.lines.assign(g_lines.begin(), g_lines.end());
            s.serial = serial;
            s.to_end = true;
        }
        float below = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##lines", ImVec2(0, -below), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav)) {
            ImGuiListClipper clipper;
            clipper.Begin(int(s.lines.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                    const std::string& line = s.lines[size_t(i)];
                    bool typed = !line.empty() && line[0] == ']';
                    if (typed) ImGui::PushStyleColor(ImGuiCol_Text, kTyped);
                    ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size());
                    if (typed) ImGui::PopStyleColor();
                }
            float page = ImGui::GetWindowHeight() * 0.8f;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
                ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - page));
                s.to_end = false;
            } else if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) {
                ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + page));
            } else if (s.to_end) {
                ImGui::SetScrollHereY(1.0f);
                s.to_end = false;
            }
        }
        ImGui::EndChild();

        ImGui::PushStyleColor(ImGuiCol_Text, kTyped);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("]");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        constexpr ImGuiInputTextFlags kInput = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter |
                                               ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
        bool entered = ImGui::InputText("##typed", s.input, sizeof(s.input), kInput, InputEvent);
        if (entered) {
            std::string line = s.input;
            while (!line.empty() && line.back() == ' ') line.pop_back();
            size_t first = line.find_first_not_of(' ');
            line = first == std::string::npos ? std::string() : line.substr(first);
            if (!line.empty()) {
                if (s.history.empty() || s.history.back() != line) s.history.push_back(line);
                if (s.history.size() > kHistory) s.history.erase(s.history.begin());
                Submit(line);
            }
            s.recalled = -1;
            s.input[0] = 0;
            s.to_end = true;
        }
        ImVec2 typed_at = ImGui::GetItemRectMin();
        Suggest(ImVec2(typed_at.x, ImGui::GetWindowPos().y + ImGui::GetWindowSize().y), kBack, kEdge, kText, kTyped);
        // the typing line keeps the keys for as long as the console is open
        ImGui::SetItemDefaultFocus();
        if (opened || entered || (!ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))
            ImGui::SetKeyboardFocusHere(-1);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(6);
}

}  // namespace app::console
#else
namespace app::console {
void Draw() {}
}  // namespace app::console
#endif
