# Third party notices

## MOHRS-XBERecomp

Parts of `src/common` (mouse_keyboard, menu, menu_pointer, options, software_cursor, game_info)
come from MOHRS-XBERecomp by neonoxd: https://github.com/neonoxd/MOHRS-XBERecomp, through the Medal of Honor: Rising
Sun port built with the same toolkit.

```
MIT License

Copyright (c) 2026 neonoxd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## CoD2-BRO-Native-Loader

The menu mouse, mouse look, controller layout following and field of view in `src/game` (menu_mouse, mouse_look,
fov) follow the reverse engineering of CoD2-BRO-Native-Loader by SmokeDoggy044
(https://github.com/SmokeDoggy044/CoD2-BRO-Native-Loader), licensed under the GNU General Public License version 2
or (at your option) any later version, used here under version 3 like the rest of this project.

## CoD2-BRO-Native-Loader

`src/game/font_atlas.h` (the game's font sheets drawn again from font files) follows `text/sharp_text.cpp` of
CoD2-BRO-Native-Loader by SmokeDoggy044, GPL-2.0-or-later, used here under GPL-3.0.

## Fonts

`fonts/` holds EB Garamond Bold, Jost Medium and Anton, unmodified, each under the SIL Open Font License 1.1; the
license texts are in `fonts/licenses/`.

`fonts/Hack-Regular.ttf` is Hack 3.003 (https://sourcefoundry.org/hack/), unmodified, the FPS counter's and the
console's typeface. Copyright 2018 Source Foundry Authors, MIT License; Bitstream Vera Sans Mono Copyright 2003
Bitstream Inc., Bitstream Vera License with Reserved Font Names "Bitstream" and "Vera"; the DejaVu changes are in the
public domain. The full texts are in `fonts/licenses/Hack-LICENSE.md`.

## SMAA

The SMAA option uses the reference shader and lookup tables of SMAA (github.com/iryoku/smaa), MIT licence:

```
Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)

Permission is hereby granted, free of charge, to any person obtaining a copy
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to
do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software. As clarification, there
is no requirement that the copyright notice and permission be included in
binary distributions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Controller button pictures

`button_images/` holds the controller button prompts (`src/game/button_images.cpp`): ButtonIcon-Xbox-*.png
(`og_*.tga`, the game's own layouts), ButtonIcon-XboxOne-*.png (`xb_*.tga`) and ButtonIcon-PS4-*.png (`ps_*.tga`, the
Modern layout), stored as .tga. SOURCE AND LICENCE TO
BE CREDITED HERE BEFORE RELEASE.
