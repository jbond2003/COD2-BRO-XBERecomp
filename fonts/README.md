# Fonts for sharp text

Free look-alikes of the game's bitmap fonts, built into the exe. The game's font sheets are drawn again from them at
the output's resolution and set in their own widths (`src/game/sharp_text.cpp`), where Windows does not have the
real face (Impact, Garamond Bold). All are under the SIL Open Font License 1.1 (texts in `licenses/`); none is
modified. Chosen by CoD2-BRO-Native-Loader, whose copies these are.

| Game font | Font here | Source |
|---|---|---|
| `garamond`, `arialblack_14` (Garamond Bold) | EB Garamond Bold (`EBGaramond-Bold.ttf`) | github.com/octaviopardo/EBGaramond12, `fonts/ttf/` |
| `GemFontOne` (Futura-like) | Jost Medium (`Jost-500-Medium.ttf`) | github.com/indestructible-type/Jost, `fonts/ttf/` |
| `impact` (Impact) | Anton (`Anton-Regular.ttf`) | github.com/google/fonts, `ofl/anton/` |

`Hack-Regular.ttf` is Hack 3.003 (sourcefoundry.org/hack, github.com/source-foundry/Hack, release
`Hack-v3.003-ttf.zip`), unmodified: the FPS counter and the console's typeface (`src/common/paper_page.cpp`). It is
under the MIT License with the Bitstream Vera License for the Bitstream Vera Sans Mono it comes from
(`licenses/Hack-LICENSE.md`).
