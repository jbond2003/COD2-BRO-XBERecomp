// Cheats: the engine's own developer commands, behind a code typed in the settings panel (F10). src/game/cheats.cpp
#pragma once

struct X86Context;

namespace game::cheats {

bool Unlocked();
// true when the code is the right one: cheats are on until the game closes
bool TryUnlock(const char* code);

// the word thereisacow1337 typed in the console: every mission is unlocked for the profile signed in (the game's
// thread). False when it is another word or no profile is signed in.
bool UnlockMissions(const char* word);

// kBackwards: moving and looking are turned round, on the sticks, the keys and the mouse
enum Toggle { kGod, kNoTarget, kLowGravity, kFastRun, kBackwards, kToggles };
bool On(Toggle t);
void Set(Toggle t, bool on);
// unlimited ammo: the player's rounds never go down while it is on
bool UnlimitedAmmo();
void SetUnlimitedAmmo(bool on);
void FullHealth();  // the player's health back to full, once

// a line for the top of the screen and the seconds since it was set; null when there has been none
const char* Message(double& age);

// the game is in its menus (the front end), not in a mission: as the game's thread last saw
bool InMenus();

// a full screen movie is playing (a logo, a cinematic), as the game's thread last saw (src/game/movies.cpp)
bool MoviePlaying();

void Tick(X86Context& ctx);  // the game's thread, each client frame

// the cutscene bars as the setting has them now (src/game/bodies.cpp); the game's thread, each client frame
void CutsceneBars(X86Context& ctx);

}  // namespace game::cheats
