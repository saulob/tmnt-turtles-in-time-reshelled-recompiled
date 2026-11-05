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
| `.deep` | `#03140D` | backdrop, tunnel interior, shell backing |
| `.dark-green-stop` | `#082318` | water |
| `.green-line` | `#0E4F30` | pipes, ambient light |
| `.emerald` | `#1F7448` | tunnel rim, fence, brick lines |
| `.accent` | `#5CE070` | launcher accent: small lights only |
| `.shell-orange` | `#FF8A1F` | shell 1 |
| `.shell-blue` | `#1E9DFF` | shell 2 |
| `.shell-purple` | `#A844FF` | shell 3 |
| `.shell-red` | `#FF2B3A` | shell 4 |

The four shells are `<use>` instances of one `#shell` group, coloured through `currentColor`, and
run left to right as orange, blue, purple, red.

## Layout

The launcher anchors the image to the header's top, right and bottom and crops it on the left, so
keep the left 55% almost empty for the title. The bottom 35% fades into the page, so anything that
must stay visible belongs above `y=125`.

# Sponsor heart

`sponsor-heart.svg` is the source of truth for the heart in the Sponsor on GitHub buttons (CC0, see
`packaging/licenses/THIRD-PARTY-LICENSES.txt`). `sponsor-heart.png` is the runtime asset, a white 64x64 rendering of it
with a transparent background. Do not edit it by hand; regenerate it after every SVG change, from the repository root
with rsvg-convert:

```sh
rsvg-convert -w 64 -h 64 -f png -o assets/launcher/sponsor-heart.png assets/launcher/sponsor-heart.svg
```
