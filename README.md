# Call of Duty 2: Big Red One - XBE Recomp

An unofficial native PC port of the original Xbox version of Call of Duty 2: Big Red One, built with
[Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp). Work in progress: the campaign boots and plays, PC features are still to come. See
[ISSUES.md](ISSUES.md) for the problems found so far and how they were fixed.

> [!NOTE]
> This project does not include any game files or recompiled game code. You need your own legally acquired copy of
> the game.

## Building

1. Clone with `git clone --recursive` (the SDK is the `sdk` submodule).
2. Put your game files in the `assets` folder: `default.xbe` with the game's folders (`media`, `sp`, ...) beside it.
3. Install Visual Studio 2022 or newer with **Desktop development with C++**, **C++ Clang tools for Windows** and
   **C++ CMake tools for Windows** (`build.bat` offers to add the last two).
4. Run `build.bat`. The first build recompiles the game and takes a while.

## Credits

- [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) by TGP482, the recompiler and runtime this is built on.
- Made with the help of [Claude](https://claude.ai).
