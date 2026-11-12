# Call of Duty 2: Big Red One - XBE Recomp

An unofficial native PC port of the original Xbox version of Call of Duty 2: Big Red One, built with
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp).

## Highlights

- **Modern controls.** The awful original Xbox controller scheme isn't the default anymore. A new Modern layout is:
  right trigger fires, left trigger aims down the sights, A jumps, B crouches (hold it to go prone), click the left
  stick to sprint and the right stick to melee. The game's four old layouts are still there if you want them.
- **Q and E leaning**, like the PC Call of Duty games, to peek around corners and over cover.
- **Full keyboard and mouse.** Mouse look, rebindable keys, the mouse in every menu, and
  button prompts that switch between keyboard and controller the moment you do.
- **High refresh rate gameplay.** Play at 60, 120, 144 or 240 fps, or with no limit at all, with VSync if you want
  it.
- **Sharp text.** The game's small, blurry bitmap fonts are drawn again from real font files, crisp at high
  resolutions.
- **No more 4:3 menus.** The front end menus fill a widescreen window instead of sitting in a box with black bars.
- **Higher resolutions and a field of view setting**, in a PC Options menu built in the game's own style.
- **Sprint in the campaign**, which the Xbox game only had in multiplayer.

## Features

The [CoD2 BRO Native Loader](https://github.com/SmokeDoggy044/CoD2-BRO-Native-Loader) was the guide for which
features this version should have, and a very helpful one. Its notes on the game were a reference for the mouse,
look and field of view work, and the sharp text is adapted from its code (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). It also does things this port does not: it runs on Linux and has
multiplayer over System Link. Thank you to its author. Thank you as well to
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp), the recompiler and runtime that this port is built on.

This one takes a different route, a static recompilation into a native 64-bit Windows program drawing with
Direct3D 12, and adds a few things of its own:

- Sprint in the campaign, which the Xbox game only had in multiplayer: hold Shift or click the left stick.
- Tutorial hints for sprinting, leaning and hiding the HUD, and the others reworded for the keyboard.
- H hides the HUD and brings it back (the key can be changed).
- The M1 Garand can be reloaded before its clip is empty.
- A built in, dedicated PC Options menu in the game's own style, with pages for display (with brightness), graphics (with view distance),
  mouse, and keyboard with key bindings; F10 opens the full settings panel.
- More lines in the game's Gameplay menu, and in the pause menu's options: controller aim assist, tutorial hints,
  and hiding the HUD.
- No title screen: the game starts at the profile menu, and the title's picture is the splash shown while the game
  loads (made from your own game the first time you start it).
- A Modern controller layout beside the game's four.
- Typing your profile name on the keyboard, on a QWERTY typewriter.
- A frame rate limit of 30, 60, 120, 144 or 240, or none, and VSync, with the cinematics at their own 30, and
  movies you can skip.
- Texture packs: replace a texture with your own image, or dump the game's.
- The port's version in the corner of the menus. The game never goes online.

Like the loader, it has 60 fps play, higher resolutions, a field of view setting, mouse look, Q and E to lean, the
mouse in the menus, sharp text, and button prompts that switch between keyboard and controller as you do.

## Known issues

- In Desert Fox, a tank in the minefield can glitch at frame rates above 60 (seen at 144). Setting Max Frame Rate
  to 60 for that mission is the thing to try until it is fixed.
- The first time you play, there can be a microstutter or two at the start while the game compiles its shaders
  for your GPU. They are saved after that, so it only happens once.
- PlayStation controllers are not supported: there are no PlayStation button prompts, and none has been tested.

See [ISSUES.md](ISSUES.md) for the problems found so far and how they were fixed.

> [!NOTE]
> This project does not include any game files or recompiled game code. You need your own legally acquired copy of
> the game.

The port is made from the "World" disc of the game (title ID 41560051, version 1), whose one executable is marked
for every console region. `build.bat` warns when your `default.xbe` is a different build: a separately built
regional release has its code at other addresses and is not supported.

## Building

1. Clone with `git clone --recursive` (the SDK is the `sdk` submodule).
2. Put your game files in the `assets` folder: `default.xbe` with the game's folders (`media`, `sp`, ...) beside it.
3. Install Visual Studio 2022 or newer with **Desktop development with C++**, **C++ Clang tools for Windows** and
   **C++ CMake tools for Windows** (`build.bat` offers to add the last two).
4. Run `build.bat`. The first build recompiles the game and takes a while.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482, the recompiler and runtime this is built on.
- [CoD2 BRO Native Loader](https://github.com/SmokeDoggy044/CoD2-BRO-Native-Loader) by SmokeDoggy044, the guide for this
  port's features and a reference for parts of it.
- jojo8479, who drew the controller picture for the Modern layout.
- Made with the help of [Claude](https://claude.ai).

<details>
<summary>Secrets (spoilers)</summary>

- **Cheats:** press F10, go to the end of the panel and enter the password `brooklyn`. The cheats stay unlocked until
  the game is closed, and they can break missions, so use them at your own risk.
- **Every mission unlocked:** create a profile named `thereisacow1337`.

</details>
