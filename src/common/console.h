// The developer console, on the key under Escape (` and ~ on a US keyboard) as in the PC Call of Duty games. The
// engine still has its command buffer, its commands and its variables; what the Xbox build lost is the console's
// screen and nearly every line it printed. The screen is drawn here (console.cpp, Windows only); the typed lines
// are run on the game's thread, which also answers for the engine where its own answer was compiled out
// (src/game/console.cpp).
#pragma once

#include <string>
#include <vector>

namespace app::console {

bool Open();
void Toggle();  // any thread
// The console only opens once a profile is signed in (his ask): the game's thread says so each frame
// (src/game/console.cpp), and an open console closes when it stops being so.
void SetAllowed(bool allowed);
bool Allowed();
void Draw();    // the UI's thread, each frame

// any thread. A text may hold several lines; the last few hundred are kept
void Print(const std::string& text);
void Clear();
// a typed line for the game's thread, and the game's thread taking it
void Submit(const std::string& line);
bool Take(std::string& line);
// what Tab completes: the game's thread hands over the engine's command and variable names when asked (the console
// asks each time it opens)
bool NamesWanted();
void SetNames(std::vector<std::string> names);

}  // namespace app::console
