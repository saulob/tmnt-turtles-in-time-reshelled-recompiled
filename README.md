<div align="center">

# Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled Recompiled

### The Xbox 360 XBLA release of Turtles in Time Re-Shelled, statically recompiled to native PC code with ReXGlue

<sub>The original Xbox 360 game code, statically recompiled to native PC code with the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue SDK</a>. <b>No game files included</b>: bring your own Turtles in Time Re-Shelled Xbox Live Arcade package.</sub>

</div>

> **Early development.** The project builds on the infrastructure of
> [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled). Code generation works for
> both game executables; the game has not been run yet and is not known to be playable. See
> [docs/project-status.md](docs/project-status.md).

## What you need

- Windows 10 or 11, 64-bit, and a DirectX 12 graphics card. Linux is planned: the build and packaging exist but have
  not been run for this game ([docs/linux.md](docs/linux.md)).
- **Your own Turtles in Time Re-Shelled Xbox Live Arcade package**, this exact game version:

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

## The launcher

The TRG launcher opens before the game. On the **Play** page, **Install from package...** takes your package (one file
with a long hexadecimal name and no extension; on an Xbox 360 drive it is in
`Content\0000000000000000\584109D2\000D0000\`), copies the game files (about 270 MB) into the `game` folder next to the
program and shows **Ready to play**. **Reinstall from package...** repairs the game files and keeps settings and saves.

Pages: Play, Display (display mode, window size, monitor, VSync), Graphics (graphics card, render
resolution, anti-aliasing, multisampling, texture filtering), Gameplay (language: English, French, German, Italian, Spanish or the port's unofficial
[Português Brasil](docs/localization-pt-br.md); frame
rate and render resolution in the title, frame counter, player names), Achievements, Controls (keyboard on/off and
bindings) and
About (versions, folders, settings file, detailed logging). Settings are saved to `user_data\settings.toml` next to the
program. **F11** toggles borderless fullscreen for the launcher and for the game. Hold **Shift** while starting to show a
launcher that was turned off. See [docs/launcher.md](docs/launcher.md).

## Building it yourself

The recompiled C++ is generated from your own game files, so the repository does not contain it. Clone with
submodules, so the TRG launcher is included:

```powershell
git clone --recurse-submodules https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled.git
```

In an existing clone, run `git submodule update --init --recursive`.

1. Install Visual Studio with the C++ and LLVM/Clang components. `tools\build.ps1` expects Visual Studio 18 Community at
   its default location.
2. Extract your package's files into `game\extracted` with an Xbox 360 STFS extractor, so that
   `game\extracted\default.xex` and `game\extracted\TMNT_r.xex` exist.
3. `.\tools\codegen.ps1` downloads the pinned ReXGlue SDK 0.10.0 (unless `tools\rexglue` already holds it), checks both
   executables and generates the C++ into `generated\default` and `generated\tmnt_r`.
4. `.\tools\build.ps1` builds `build\tmnt_turtles_in_time_reshelled.exe` and the module library
   `build\tmnt_turtles_in_time_reshelled_TMNT_r.dll`. It first makes sure ReXGlue's runtime and Xenos GPU plugin in
   `tools\rexglue` were built from this project's patches (`patches\rexglue-sdk`) and rebuilds them from the v0.10.0
   source when they were not (network access the first time). See
   [docs/rexglue-runtime-patch.md](docs/rexglue-runtime-patch.md).
5. `.\tools\package-release.ps1` makes the release ZIP in `dist\`. It publishes nothing.

How the launcher, settings and installer work is described in [docs/launcher.md](docs/launcher.md), and the user data
layout in [docs/user-data.md](docs/user-data.md).

## Reporting problems

Attach the newest `tmnt_turtles_in_time_reshelled_*.log` from the `logs` folder next to the program to an
[issue](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues). After a crash on Windows the log ends
with a `CRASH REPORT` section.

## Credits

- Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled for Xbox Live Arcade (2009). This is an unofficial fan-made
  PC port. It is not affiliated with or endorsed by the game's developer, publisher or rights holders, and it contains
  none of their files.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) for the recompiler and runtime (BSD 3-Clause)
- [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), the launcher interface, pinned at v1.1.0, licensed under
  the MIT License
- The project infrastructure comes from [Hard Corps: Uprising Recompiled](https://github.com/saulob/hard-corps-uprising-recompiled)
- The licenses of the libraries the release includes are in the `licenses` folder of the ZIP

## AI disclosure

AI tools were used to assist with code analysis, development, debugging and documentation. Changes were directed,
tested and reviewed by the maintainer.

## License

Original code in this project is licensed under the GNU General Public License v2.0 only (GPL-2.0-only). See
[LICENSE](LICENSE).

Third-party components retain their respective licenses and notices. See
[licenses/ReXGlue-LICENSE.txt](packaging/licenses/ReXGlue-LICENSE.txt) and
[licenses/THIRD-PARTY-LICENSES.txt](packaging/licenses/THIRD-PARTY-LICENSES.txt).

No game files are included. Turtles in Time Re-Shelled and its assets are not covered by this project's license.
