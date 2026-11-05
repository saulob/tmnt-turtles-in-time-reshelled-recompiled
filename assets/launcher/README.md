# Launcher header art

`header-art.svg` is the source of truth. Edit it first.

`header-art.png` is the runtime asset, generated from the SVG at 3840x432 with a transparent
background. Do not edit it by hand; regenerate it after every SVG change.

From the repository root on Windows, with Edge installed:

```powershell
& 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe' `
  --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 `
  --window-size=3840,432 "--screenshot=$(Join-Path $PWD 'assets\launcher\header-art.png')" `
  "file:///$((Resolve-Path assets/launcher/header-art.svg).Path -replace '\\','/')"
```

`--screenshot` needs an absolute path.

## Palette

Every colour is one line at the top of the SVG's `<style>`; change it there and regenerate.

| Class | Colour | Use |
| --- | --- | --- |
| `.black` | `#03140D` | tunnel interior |
| `.deep` | `#082318` | floor |
| `.green` | `#0E4F30` | pipes, ambient light |
| `.emerald` | `#1F7448` | tunnel rim, fence, brick lines |
| `.glow` | `#5CE070` | launcher accent: small lights only |
| `orange-stop` | `#FF8A1F` | streak 1 |
| `blue-stop` | `#1E9DFF` | streak 2 |
| `purple-stop` | `#A844FF` | streak 3 |
| `red-stop` | `#FF2B3A` | streak 4 |

The streaks run bottom to top as orange, blue, purple, red.

## Layout

The launcher anchors the image to the header's top, right and bottom and crops it on the left, so
keep the left 55% almost empty for the title. The bottom 35% fades into the page, so anything that
must stay visible belongs above `y=125`.
