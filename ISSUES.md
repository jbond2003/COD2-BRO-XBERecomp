# Problems hit so far, and their fixes

What went wrong while bringing Call of Duty 2: Big Red One up on Xbox XBE Recomp, why, and how it was fixed. Written
for anyone porting another title with the toolkit: most of these are not specific to this game.

Addresses are in the `default.xbe` this repo is configured for. Names come from reverse engineering, not from
any source code.

## 1. Script threads crash with a `pushf` trap (fixed)

**Symptom.** The first level script that waits (`wait 1;`) stops the game with a trap at 0x11D990.

**Cause.** The level scripts are native code run as coroutines. A waiting script pushes its registers, copies its part of
the guest stack out (Scr_SaveThreadLocals) and long jumps back to Scr_ResumeThread 0x2FE100. Later the stack is copied
back to the same address and the script returns into the middle of its function. Recompiled code also keeps frames on
the host stack, so a guest long jump cannot unwind into the middle of a host function.

**Fix.** `src/game/script_threads.cpp` runs every script thread on a host fiber of its own (`sdk/runtime/core/fiber.*`,
Windows fibers or ucontext). A wait switches back to the scheduler fiber and a resume switches into the script again.
The guest still copies its stack so the game's own bookkeeping matches.

**Lesson.** Look for `setjmp`/`longjmp` pairs that save and restore part of the stack; they need host fibers.

## 2. Indirect call to unknown code at 0x14370 (fixed)

**Cause.** A function reached only through a pointer table the scan did not see.

**Fix.** Add it to `[functions]` in `config/game.toml`. A scan of stored code pointers found two more (0x72E50,
0x6F6500) before they were hit.

## 3. Black screen in game and black movies on Windows (fixed)

**Symptom.** Headless frames looked right but the Windows build showed black for the movies and in game.

**Cause.** 0x3EB910 was named `D3DResource_IsBusy` in the config but is `D3D_BlockOnResource`. The D3D12 backend only
uploads what the CPU wrote into a render target or texture after the BlockOnResource HLE marks it dirty, so CPU decoded
movie frames never reached the GPU.

**Fix.** `D3D_BlockOnResource = 0x3EB910` with `abi = ["ecx"]`.

**Lesson.** When the headless reference renderer is right and D3D12 is black, check the resource sync functions first.

## 4. Movies play at double speed at 60 fps (fixed)

**Cause.** The movie decoder (Treyarch "AFMV" `.xbv`) shows one frame per game frame. On the Xbox the game held the
display to every second vblank, so 30 fps. With an uncapped or 60 fps frame rate the movies run twice as fast.

**Fix.** `src/game/movies.cpp` wraps the decode function (vtable 0x7CF390 slot 2) and calls
`app::LimitNextFrame(30)`, so frames with a movie are paced to 30 fps and gameplay keeps its own limit.

## 5. "Profile appears to be damaged" (fixed)

**Cause.** The game opens save files for overlapped I/O and treats a ReadFile that returns TRUE at once as the end of
the file. The Xbox hard disk always returns STATUS_PENDING. The runtime completed host reads synchronously, so the game
read 0x400 bytes and declared the profile corrupt.

**Fix.** The SDK returns STATUS_PENDING (with the status block already final) for host files opened without
`FILE_SYNCHRONOUS_IO_*`.

## 6. Player pushed into the jeep, camera inside debris, models flattened at some angles (fixed)

**Symptom.** After the opening briefing the player is pushed inside the jeep and killed by artillery. Standing or going
prone next to small debris puts the camera inside it. Some models squash flat when seen from certain angles.

**Cause.** The game sets the SSE rounding mode to truncation at boot (`ldmxcsr` at 0x64BF9, `or eax, 0x6000`). Its fast
sin/cos (0x110F0, used by AnglesToAxis 0x2A2AF0 and much else) rounds by adding and subtracting 1.5 × 2^23 and depends
on that mode for its range reduction. The recompiler stored the new MXCSR but generated code always ran with the host's
round to nearest, so sin/cos were wrong by up to a quarter turn for many angles. Collision traces, bounding boxes and
model axes were then built from bad vectors.

**Fix.** In the SDK: `ldmxcsr` now loads the guest's rounding and flush to zero bits into the host MXCSR while guest
code runs. Host code (HLE functions, kernel exports, the renderers) is switched back to round to nearest on entry, and
`CallGuest` switches to the guest's mode. Fibers carry the mode across a switch.

**Lesson.** If geometry or physics is slightly or badly wrong while everything else works, test the game's math helpers
directly from a hook against known values, and grep the disassembly for `ldmxcsr` and `fldcw`.

## Build and setup issues

- **`no member named LimitNextFrame` when building.** `git pull` does not update the `sdk` submodule. `build.bat` now
  runs `git submodule update --init --recursive` itself. If it still fails, run that command once by hand. Windows
  PowerShell 5 does not accept `&&`, so run `git pull` and the submodule command as two lines.
- **No `assets` folder after cloning.** The folder is now in the repo with a `README.txt` saying what goes in it.
