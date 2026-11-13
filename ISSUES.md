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

## 7. Keyboard and mouse: what had to be found

Not bugs, but things another port will meet.

- **Mouse look.** The Xbox build feeds its right stick through Quake 3's mouse path (CL_MouseMove 0x2B39E0, with
  stick acceleration and a turn limit), so a real mouse cannot go through it. `src/game/mouse_look.cpp` adds the mouse
  to cl.viewangles before CL_MouseMove runs, under the same conditions (view locks, vehicles, freelook, invert).
- **Controller layouts.** The keyboard gives the default layout's buttons; the layouts in the game's options bind
  them to other commands, so each key's button is moved to the one bound to its command now (key bindings at
  0x93425C).
- **Menu pointer.** The menus have no mouse support at all. Every drawn text widget (draw 0x321160) is recorded with
  its box, and the pad poll (0x31FC10) selects the item under the pointer and taps A, B, up/down or left/right as a
  controller would. Picture-only buttons (the typewriter's backspace, shift, space and done) have no text, so they
  get fixed boxes.
- **Typewriter.** Its key caps are background art, and the letter typed comes from the key's index (0x325980), not
  its label. QWERTY order therefore needs the index remapped as well as the labels swapped.
- **Button prompts.** The buttons are pictures: an icon font (`i_button_icons_xb_<lang>`, glyphs 33-49) and four
  footer icons (`Xbox_A/B/X/YButton.dds`) in `sp/global.cod`. The SDK now lets a game draw its own picture in place of
  a texture (`gpu/texture_override.h`). `src/game/prompts.cpp` matches those textures by their bytes and draws
  keycaps with the bound keys' names while the keyboard and mouse are in use.
- **A tank flew into the air at 200 to 300 fps.** The prologue's first Sherman, about 11 seconds in. A vehicle's wheel
  that finds ground above it is put on it at once, and the next frame's search starts from the raised body, so a wheel
  against something tall climbed about 70 units every frame it touched it: one or two frames at 30 fps, twenty at
  300. `src/game/high_fps.cpp` (hook at `0x26AFBD`) limits the climb to a rate per second.
- **The player crept while standing still at 120 fps and above.** After every step the game pushes the player away
  from what stands near and out of what it touches (`0x2E6260`), by distances that ignore the frame's length, so
  the creep grew with the frame rate. `src/game/high_fps.cpp` runs that push 30 times a second, as on the console. One push still moved a
  standing player up to 0.07 units toward the map's origin (a unit a second on The Desert Fox's dunes), so a push
  shorter than an eighth of a unit is taken back while the player stands still.

- **The controller's look sensitivity was lost on quitting.** The Controller screen's Apply and the pause menu's
  Options set it in the profile, but the game writes its profile only when it is made or a mission is finished.
  `src/game/pad_layout.cpp` keeps it in the settings file and puts it back in the profile whenever one is applied.

- **A tank's turret turned slowly by itself, faster at high frame rates.** `CL_FinishMove` (0x2ABB30) makes the
  command's 16 bit angles as the whole part of steps + 0.5 cut toward zero, which is one step (0.0055 degrees) short
  for an angle below zero. In a vehicle the view is written back from the command's angles every frame
  (`CG_CalcVehicleViewAngles`, 0x2864A0), so such an angle lost a step a frame: 0.16 degrees a second at 30 frames a
  second, 0.8 at 144. A hook at 0x2ABB6C (`src/game/high_fps.cpp`) rounds an angle below zero to the nearest step.

- **Bonus Materials: white pictures, "BONUS_VEH_3_0_INFO" texts, arrows that did not click.** Each Bonus page does its
  work in its own up and down (vtable `+0x50` / `+0x54`): a category's first item set to 1 (0 until then), its texts
  and picture loaded. The mouse selected lines through the plain select (`+0x88`), so a category picked by the mouse
  kept item 0 and asked for files like `vh3_0.dds` that do not exist (a white picture and a white full screen) and
  for string names that do not exist. `src/game/menu_mouse.cpp` now steps the selection through the page's own up
  and down. A click on a picture's arrows was taken as a click on the list line at the same height (the "value right
  of its label" rule); that rule is off on Bonus pages. A click on a line picks it and opens it full screen (his ask).
  The full screen view moves its prompts 200 right of the widget's place, so the Back click now follows where the
  prompt's pieces are drawn. The Characters, Weapons and Vehicles prompts sit under the list on the left, clear of
  the text.

## 8. No way to delete a profile (fixed)

**Symptom.** The Profile Manager only loads or makes profiles; nothing deletes one.

**Cause.** The game has a delete: X on the profile screen asks "Are you sure you wish to delete this save file?"
(0x334A20) and rewrites the Profiles files. But it only answers in the screen's mode 0, which is Campaign > Save
Profile, and only there does the foot of the screen say "X to delete".

**Fix.** Two instruction hooks (`src/game/profiles.cpp`) let the Profile Manager (mode 1) and Switch Profile (mode 2)
take the same path: the foot text (0x33A64D) and X (0x334A31). The confirmation, the write and "Delete Successful" are
the game's own. On the keyboard Delete is X in menus, and the X keycap reads Del there.

## 9. A soldier shot far away stays a few flat shapes when walked up to (fixed)

**Cause.** The game's own. A soldier's model has up to five levels of detail, picked each game frame by distance
(G_UpdateEntityLOD 0x2B8450: level 2 from 240 units, 3 from 480, 4 from 720). R_AddDObjSurfaces 0x244020 never draws
a model finer than the level its pose was last worked out for. A living soldier's pose is worked out again each
frame; a body the physics moves is not (DObj_UpdateSkeletons 0x2DF880 leaves at once for entity flag 0x400000), and
some bodies keep that flag for good, so they keep the level the soldier had as he fell.

**Fix.** `src/game/detail.cpp`: such a body is drawn at the level its distance asks for. The physics fills every
bone (every level of these models has the same 80), which a run in the Oran fight confirmed for each bone of each
such body.

## 10. Sounds that belong in the middle jump to one side for a moment (fixed)

**Cause.** Two in the toolkit's DirectSound. The game sets its listener and every 3D voice with DS3D_DEFERRED and
commits once (0xC7D16); the settings were applied at once and the commit did nothing, so the mixer, on its own
thread, mixed blocks with the listener moved and the voices not. And the delay between the ears was the one for a
sound level with the head whatever its height, so a sound at the listener's feet or overhead had a side.

**Fix.** The toolkit keeps deferred settings until the commit, and shortens the ear delay by the cosine of the
elevation, as the console's own filter table does (word[61 x 31] at 0x3E35E8 into 0x20 byte filters at 0x3D2028).

**Not a fault: voices against gunfire.** Measured in Oran: the player's own shots are 2D sounds the game plays at
0.19 of full volume, dialogue is 3D at 0.92 with a 25000 unit minimum distance. That balance is the game's.

## 11. Shadows are soft blobs (fixed)

**Cause.** The toolkit's. The game draws its shadow casters into one 512x512 picture (R_RenderTestShadow 0x241290,
700 units across) and lays it on the world through a texture stage, one linear tap. The D3D12 renderer gives a
target read into a smaller target the glow chain's box taps (three console texels each), and took "smaller" as
smaller in either axis: 512 is taller than the 480 line frame. Every shadow was averaged over three texels, about
10 cm, at 16 taps a pixel at 1080p.

**Fix.** The box is for a target with more texels than the one drawn to. Shadows are one tap of a picture at
render scale again, and every draw that reads them is cheaper.

**Lesson.** A draw list of every stage's texture, with its filters, beside each target's size finds this in one
frame: the harness's `CAP=N` with a few more lines printed.

## 12. Black screen after the opening movies on the Steam Deck (fix not yet tested on a Deck)

**Cause.** The renderer builds its shaders at run time with `D3DCompile`. Windows has Microsoft's
`d3dcompiler_47.dll`; Proton and Wine have their own, which has no `1.#INF` literal. The vertex shaders' helper
lines use it, so every one failed (`d3d12: vs_5_0 compile failed: ... E5000: syntax error`, in a player's log)
and only the movies, drawn with a small shader of their own, appeared.

**Fix.** The toolkit writes infinity by its bits (`asfloat(0x7F800000u)`), and `release.bat` puts Microsoft's
redistributable `d3dcompiler_47.dll` beside the executable.

## Still open: tanks glitch in Desert Fox's minefield at high frame rates

**Symptom.** In Desert Fox, at a frame rate limit above 60 (seen at 144), a tank in the minefield glitches: one of
its wheels sits about 250 units under the ground, the same way every time, and the tank lurches with it.

**What is known.** The game moves vehicles once per frame, so it runs more of those steps at a high frame rate. Two
attempts at a fix (the second made each wheel look for the real ground every frame) did not help and were taken
back out: the second sank the Stuarts. The prologue's Sherman, which used to fly at high frame rates, is fixed and
unaffected. Until this is solved, setting Max Frame Rate to 60 for that mission is the thing to try.

## Build and setup issues

- **`no member named LimitNextFrame` when building.** `git pull` does not update the `sdk` submodule. `build.bat` now
  runs `git submodule update --init --recursive` itself. If it still fails, run that command once by hand. Windows
  PowerShell 5 does not accept `&&`, so run `git pull` and the submodule command as two lines.
- **No `assets` folder after cloning.** The folder is now in the repo with a `README.txt` saying what goes in it.
- **Windows Defender reports Trojan:Win32/Bearfoos.A!ml and stops the game while it loads a mission.** A guess by
  Defender's machine learning scan, not a virus: it came with the online update check (a WinHTTP request to this
  repository's releases page), on top of the game starting the compiler and loading a DLL it built. The update check
  is off from v0.1 on. If Defender still stops your own build, allow it under Windows Security, Protection history.
