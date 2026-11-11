# Navigation icons

The canonical navigation icon set for the launchers. Filenames are semantic, so the folder can be copied to another
project as it is.

The shared icons match `dragons-crown-pro-pc/assets/launcher/icons`. `cheats.svg` is Dragon's `cheats.svg`, and
`achievements.svg` is Dragon's trophies.svg renamed for this launcher's page name.

**Copy `SOURCES.md` with the icons.** `display.svg` and `bug-report.svg` require attribution, and that attribution has to
go wherever the icon goes.

## Attribution required

| File | Title | Author | Collection | Source | License |
|---|---|---|---|---|---|
| `display.svg` | Display Television Tv Screen Gadget | nickylimyeanfen | Inc Device Icons | https://www.svgrepo.com/svg/381247/display-television-tv-screen-gadget | CC Attribution License |
| `bug-report.svg` | Bug File | Vectopus | Atlas Variety Line Icons | https://www.svgrepo.com/svg/456346/bug-file | MIT License |

`display.svg` is not CC0 and not public domain. License: https://creativecommons.org/licenses/by/

Adapted for the launcher: renamed from `display-television-tv-screen-gadget-svgrepo-com.svg` and drawn scaled, in the
sidebar's colours. The file's contents are unchanged.

`bug-report.svg` is under the MIT License; its notice is in `packaging/licenses/THIRD-PARTY-LICENSES.txt`. Adapted for the
launcher: renamed from `bug-file-svgrepo-com.svg` and drawn scaled, in the sidebar's colours. The file's contents are
unchanged.

## CC0

CC0 asks for no attribution; the sources are kept for provenance.

| File | Title | Source | License |
|---|---|---|---|
| `play.svg` | Play | https://www.svgrepo.com/svg/522228/play | CC0 |
| `graphics.svg` | Controls Alt | https://www.svgrepo.com/svg/348105/controls-alt | CC0 |
| `gameplay.svg` | Gamepad | https://www.svgrepo.com/svg/523439/gamepad | CC0 |
| `online.svg` | Globe | https://www.svgrepo.com/svg/522410/globe | CC0 |
| `controls.svg` | Keyboard | https://www.svgrepo.com/svg/524665/keyboard | CC0 |
| `achievements.svg` | Achievements | https://www.svgrepo.com/svg/304482/achievements | CC0 |
| `about.svg` | About | https://www.svgrepo.com/svg/486929/about | CC0 |
| `dlc.svg` | Download box | Original artwork for this port | CC0 |
| `cheats.svg` | Star Alt 1 | https://www.svgrepo.com/svg/532289/star-alt-1 | CC0 |

## User-provided source

| File | Title | Source | License |
|---|---|---|---|
| `snes.svg` | Super Nintendo Controller | [assets/launcher/icons/source/super-nintendo-controller-svgrepo-com.svg](source/super-nintendo-controller-svgrepo-com.svg), from https://www.svgrepo.com/svg/82749/super-nintendo-controller | CC0 |

## Changes from the downloaded files

All files have LF line endings and their canonical names. `online.svg` also lost its white background rectangle and
the drop-shadow filter on its lower line, because neither can be tinted. No path was changed, and each file keeps the
colours it was published with: the launcher uses only their shape. `snes.svg` is the supplied local file unchanged.

The launcher draws these icons through `src/page_icons.cpp`; the SVGs are embedded in the executable and tinted with the
sidebar's text colours, so the same file works for the selected and unselected rows.
