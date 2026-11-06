<div align="center">

# Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled Recompiled

### The Xbox 360 XBLA release of Turtles in Time Re-Shelled, statically recompiled to native PC code with ReXGlue

<sub>The original Xbox 360 game code, statically recompiled to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own Turtles in Time Re-Shelled Xbox Live Arcade package.</sub>

</div>

This is a native PC port, not an emulator: the game's own PowerPC code is translated into C++ and compiled for your PC.
It runs on Windows, with an experimental Linux AppImage, and adds a launcher, online play with friends, higher render
resolutions, a Brazilian Portuguese translation and offline achievements.

<img width="1278" height="766" alt="2026-11-06_15-06_1" src="https://github.com/user-attachments/assets/a51823f7-f766-4248-af85-f6b8419a40d2" />

## What you need

- **Windows:** Windows 10 or 11, 64-bit, and a DirectX 12 graphics card.
- **Linux (experimental):** x86_64 with glibc 2.31 or newer and a working Vulkan driver, see [Linux](#linux-appimage-experimental).
- **You must own the game.** Your own Turtles in Time Re-Shelled Xbox Live Arcade package, this exact game version:

| | |
| --- | --- |
| Game | Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled, Xbox Live Arcade |
| Title ID | `584109D2` |
| Content type | `000D0000` (Xbox Live Arcade), STFS `LIVE` package |
| Media ID | `23B57DFA` |
| Game version | `0.0.1.3` |
| `default.xex` | 4,808,704 bytes, CRC32 `FC04059D`, SHA-256 `5FF5B37E120B1F2C3057D78BFC8C8AD6885C3FA576AECB3026E23E6044C5C3DE` |
| `TMNT_r.xex` | 2,002,944 bytes, CRC32 `2D0061F9`, SHA-256 `C6C19BD1D3DE0C0AF3651DE201E8CAC47D9C380C03060734F6F649E83EE89DD9` |

The launcher checks the Title ID and content type when you pick the package, and checks both executables after
extracting it. Any other version is refused, because the port is built for this exact game code. Details are in
[docs/package-analysis.md](docs/package-analysis.md).

## Installing on Windows

1. Download **TurtlesInTimeReshelled-v…-windows-x64.zip** from the
   [releases page](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/releases).
2. Extract it anywhere you can write to, not `Program Files`.
3. Run `tmnt_turtles_in_time_reshelled.exe`. The launcher opens.
4. On the **Play** page, **Install from package...** takes your package (one file with a long hexadecimal name and no
   extension; on an Xbox 360 drive it is in `Content\0000000000000000\584109D2\000D0000\`). It copies the game files
   (about 270 MB) into the `game` folder next to the program and shows **Ready to play**.
5. Press **PLAY**.

**Reinstall from package...** repairs the game files and keeps settings and saves.

**Windows SmartScreen** may warn the first time, because the program is not digitally signed. If you downloaded it from
the releases page above, choose **More info**, then **Run anyway**.

## Linux AppImage (experimental)

Download **TurtlesInTimeReshelledRecompiled-v…-x86_64.AppImage**, make it executable (`chmod +x`) and run it from a folder
you can write to. This is a native Linux build, not Proton or Wine. The launcher and its features work as on Windows, and
`game`, `logs` and `user_data` are created next to the AppImage file, so they survive moving or updating it. Rendering
goes through Vulkan.

The Linux build has not yet been validated on real hardware, on Linux or on Steam Deck. Requirements, the Steam
setup, the FUSE fallback and the known differences are in [docs/linux.md](docs/linux.md).

## The launcher

The TRG launcher opens before the game. Settings are saved to `user_data\settings.toml` next to the program.

| Page | What it holds |
| --- | --- |
| Play | Game files, Install from package, PLAY |
| Display | Display mode, window size, monitor, VSync |
| Graphics | Graphics card, Render resolution 1x to 4x, anti-aliasing, multisampling (MSAA), texture filtering |
| Gameplay | Language, Skip intro, frame rate and render resolution in the title, frame counter, player names |
| Online | Online play, Connection test, friends, invites |
| Achievements | Your achievements on this PC |
| Controls | Keyboard on/off and bindings |
| About | Versions, folders, Open logs folder, Detailed logging, Sponsor |

Hold **Shift** while starting to show a launcher that was turned off. See [docs/launcher.md](docs/launcher.md).

## Controls and hotkeys

Xbox controllers work without setup, and the keyboard works too (the Controls page lists and rebinds the keys).

| Key | Action |
| --- | --- |
| **F2** | Show or hide the FPS counter |
| **F3** | Show or hide Ping while Online play is on |
| **F7** | Open the achievements |
| **F11** | Switch between Windowed and Borderless Fullscreen |

## Online multiplayer

Play with up to three friends through the game's own Multiplayer menus. Turn on **Online play** on the launcher's Online
page, run the **Connection test**, and add friends by friend code. In the game, Multiplayer > Online offers Quick Match,
Create Match and Custom Match, and **X Invite Friends** in your lobby invites a friend.

Peers connect directly over UDP when they can (**Direct**) and through the online service's relay for any pair that
cannot (**Relay**). The online service only sees what it needs to bring players together; see
[docs/online.md](docs/online.md), which also covers what it stores and the Xbox LIVE menus this port removes.

## Languages

The game's own English, French, German, Italian and Spanish, plus the port's unofficial
[Português Brasil](docs/localization-pt-br.md) translation of the game text and the launcher. Choose on the Gameplay page.

## Skip intro, render resolution and achievements

- **Skip intro** goes straight to the title screen.
- **Render resolution** draws the game at 1x to 4x (1280×720 to 5120×2880) and scales it to the window. The game keeps its
  original 16:9 picture.
- **Achievements** work offline and are saved on this PC, with the game's own names, icons and gamerscore.

## Screenshots

<img width="1279" height="762" alt="2026-11-06_15-08" src="https://github.com/user-attachments/assets/40f9cf3e-e8b5-4cb4-954a-9404a35d91fa" />

## Your data

Everything the port writes stays next to the program (next to the AppImage on Linux), so moving that folder moves
everything. Release downloads never include any of it.

| Folder | What it holds |
| --- | --- |
| `game` | The game files from your package |
| `user_data` | Settings, saves, achievements, shader cache ([docs/user-data.md](docs/user-data.md)) |
| `logs` | One log per session |

**Open logs folder** is on the About page. **Detailed logging** is on by default; turned off, the log keeps only what
identifies the session and what went wrong.

## Updates

The launcher checks the project's GitHub releases once per start. When a newer release exists an update banner appears;
nothing is installed until you choose **UPDATE NOW** and then **RESTART TO UPDATE**. Game files, `user_data`, logs and
caches are kept. On Linux this replaces the AppImage file, which must keep its release file name.

## Known limitations

- The picture is 16:9.
- Leaderboards, Xbox Live Party and Gamer Cards are removed from the menus: this port has no Xbox Live service.
- Linux: two Direct3D 12 rendering fixes (a cyan flicker on the turtles, stale geometry) have no Vulkan version yet.
- If a player closes the window during an online match, the others can wait up to 30 s before the match goes on.

## Reporting problems

Attach the newest `tmnt_turtles_in_time_reshelled_*.log` from the `logs` folder next to the program to an
[issue](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues). After a crash the log ends with a
`CRASH REPORT` section.

## Building it yourself

The recompiled C++ is generated from your own game files, so the repository does not contain it. Clone with
submodules, so the TRG launcher is included:

```powershell
git clone --recurse-submodules https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled.git
```

In an existing clone, run `git submodule update --init --recursive`.

1. Install Visual Studio with the C++ and LLVM/Clang components. `tools\build.ps1` expects Visual Studio 18 Community at
   its default location. Release packages also need MSYS2 with `mingw-w64-clang-x86_64-toolchain` for the update helper.
2. Extract your package's files into `game\extracted` with an Xbox 360 STFS extractor, so that
   `game\extracted\default.xex` and `game\extracted\TMNT_r.xex` exist.
3. `.\tools\codegen.ps1` downloads the pinned ReXGlue SDK 0.10.0 (unless `tools\rexglue` already holds it), checks both
   executables and generates the C++ into `generated\default` and `generated\tmnt_r`.
4. `.\tools\build.ps1` builds `build\tmnt_turtles_in_time_reshelled.exe` and the module library
   `build\tmnt_turtles_in_time_reshelled_TMNT_r.dll`. It first makes sure ReXGlue's runtime and Xenos GPU plugin in
   `tools\rexglue` were built from this project's patches (`patches\rexglue-sdk`) and rebuilds them from the pinned
   source when they were not (network access the first time). See
   [docs/rexglue-runtime-patch.md](docs/rexglue-runtime-patch.md).
5. `.\tools\package-release.ps1` makes the release ZIP in `dist\`, and `tools/package-linux.sh` the AppImage (see
   [docs/linux.md](docs/linux.md)). Neither publishes anything.

How the launcher, settings and installer work is described in [docs/launcher.md](docs/launcher.md), and the user data
layout in [docs/user-data.md](docs/user-data.md).

## Credits

- Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled for Xbox Live Arcade (2009). This is an unofficial fan-made
  PC port. It is not affiliated with or endorsed by the game's developer, publisher or rights holders, and it contains
  none of their files.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) for the recompiler and runtime (BSD 3-Clause)
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, licensed under
  the MIT License
- The project infrastructure comes from [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled)
- The licenses of the libraries the release includes are in the `licenses` folder of the ZIP, and in
  `usr/share/doc/tmnt_turtles_in_time_reshelled` inside the AppImage

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed,
tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 only (GPL-2.0-only). See
[LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See
[licenses/ReXGlue-LICENSE.txt](packaging/licenses/ReXGlue-LICENSE.txt) and
[licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).

No game files are included. Turtles in Time Re-Shelled and its assets are not covered by this project's license. You must
own the game to use this port.
