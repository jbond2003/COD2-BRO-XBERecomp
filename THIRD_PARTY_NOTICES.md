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

## XNA Game Studio Button Images

`button_images/xboxController*.tga` (A, B, X, Y, Start, Back, the dpad) are pictures of the Xbox 360 controller's
buttons from the Button Images sample of XNA Game Studio
(© 2007 Microsoft Corporation), from the XNA Game Studio Archive by Simon (Darkside) Jackson
(https://github.com/SimonDarksideJ/XNAGameStudio, MIT License below; the sample at
https://github.com/SimonDarksideJ/XNAGameStudio/tree/archive/Samples/ButtonImages),
the sample's own .tga files, unmodified, with its licence (`button_images/Microsoft Permissive License.rtf`). The
Modern controller layout's button prompts draw them (`src/game/button_images.cpp`). The licence grants no right to
Microsoft's names, logos or trademarks.

```
Microsoft Permissive License (Ms-PL)

This license governs use of the accompanying software. If you use the software, you accept this license. If you do
not accept the license, do not use the software.

1. Definitions

The terms "reproduce," "reproduction," "derivative works," and "distribution" have the same meaning here as under
U.S. copyright law.

A "contribution" is the original software, or any additions or changes to the software.

A "contributor" is any person that distributes its contribution under this license.

"Licensed patents" are a contributor's patent claims that read directly on its contribution.

2. Grant of Rights

(A) Copyright Grant- Subject to the terms of this license, including the license conditions and limitations in
section 3, each contributor grants you a non-exclusive, worldwide, royalty-free copyright license to reproduce its
contribution, prepare derivative works of its contribution, and distribute its contribution or any derivative works
that you create.

(B) Patent Grant- Subject to the terms of this license, including the license conditions and limitations in section
3, each contributor grants you a non-exclusive, worldwide, royalty-free license under its licensed patents to make,
have made, use, sell, offer for sale, import, and/or otherwise dispose of its contribution in the software or
derivative works of the contribution in the software.

3. Conditions and Limitations

(A) No Trademark License- This license does not grant you rights to use any contributors' name, logo, or trademarks.

(B) If you bring a patent claim against any contributor over patents that you claim are infringed by the software,
your patent license from such contributor to the software ends automatically.

(C) If you distribute any portion of the software, you must retain all copyright, patent, trademark, and attribution
notices that are present in the software.

(D) If you distribute any portion of the software in source code form, you may do so only under this license by
including a complete copy of this license with your distribution. If you distribute any portion of the software in
compiled or object code form, you may only do so under a license that complies with this license.

(E) The software is licensed "as-is." You bear the risk of using it. The contributors give no express warranties,
guarantees or conditions. You may have additional consumer rights under your local laws which this license cannot
change. To the extent permitted under your local laws, the contributors exclude the implied warranties of
merchantability, fitness for a particular purpose and non-infringement.
```

The XNA Game Studio Archive's own licence (https://github.com/SimonDarksideJ/XNAGameStudio/blob/archive/LICENSE):

```
The MIT License (MIT)

Copyright (c) 2016 Simon (Darkside) Jackson

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

## Xelu's controller prompts

`button_images/360_LB.tga`, `360_RB.tga`, `360_LT.tga` and `360_RT.tga` (the Modern layout's LB, RB, LT and RT) are
360_LB.png, 360_RB.png, 360_LT.png and 360_RT.png by Nicolae Xelu Berbece (@xelubest), stored as .tga, otherwise
unmodified. Released under Creative Commons 0 (CC0): https://creativecommons.org/publicdomain/zero/1.0/
