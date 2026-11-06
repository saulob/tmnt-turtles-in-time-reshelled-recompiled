## TMNT Turtles in Time Re-Shelled Recompiled v0.1.0

The first public release of an unofficial native PC port of *Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled* for Xbox Live Arcade. The game's own Xbox 360 code is statically recompiled to native PC code with the ReXGlue SDK, so this is a native program and not an emulator.

**No game files are included.** You need your own Xbox 360 copy of the game, in the exact version listed below.

## Downloads

| Platform | File |
| --- | --- |
| Windows x64 | `TurtlesInTimeReshelled-v0.1.0-windows-x64.zip` |
| Windows x64 SHA-256 | `TurtlesInTimeReshelled-v0.1.0-windows-x64.sha256` |
| Linux x86_64 (experimental) | `TurtlesInTimeReshelledRecompiled-v0.1.0-x86_64.AppImage` |
| Linux x86_64 SHA-256 | `TurtlesInTimeReshelledRecompiled-v0.1.0-x86_64.sha256` |

## Install on Windows

1. Extract the ZIP anywhere you can write to, not `Program Files`, and run **tmnt_turtles_in_time_reshelled.exe**.
2. On the launcher's **Play** page, click **Install from package...** and pick your own Turtles in Time Re-Shelled Xbox Live Arcade package.
3. Wait for **Ready to play**, then press **PLAY**.

Only this game version is supported:

| | |
| --- | --- |
| Title ID | `584109D2` (Xbox Live Arcade, content type `000D0000`) |
| Game version | `0.0.1.3` |
| `default.xex` | 4,808,704 bytes, SHA-256 `5FF5B37E120B1F2C3057D78BFC8C8AD6885C3FA576AECB3026E23E6044C5C3DE` |
| `TMNT_r.xex` | 2,002,944 bytes, SHA-256 `C6C19BD1D3DE0C0AF3651DE201E8CAC47D9C380C03060734F6F649E83EE89DD9` |

The launcher checks your package and explains if it is a different version.

## Linux AppImage (experimental)

**Linux support is experimental.** The AppImage has not yet been validated on real Linux hardware or Steam Deck, so rendering, audio, controllers and online play on Linux are unconfirmed.

It needs x86_64 Linux, glibc 2.31 or newer and a working Vulkan driver. Run `chmod +x` on the AppImage and start it from a folder you can write to. This is a native Linux build, not Proton or Wine, and rendering goes through Vulkan. The launcher and the **Install from package...** flow are the same as on Windows, and `game`, `logs` and `user_data` are created next to the AppImage file. The [Linux guide](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/blob/v0.1.0/docs/linux.md) covers requirements, Steam setup and the automatic fallback for systems where FUSE cannot mount the AppImage.

## What's in it

**Platforms and launcher**
- Native Windows x64 build, and an experimental native Linux x86_64 AppImage
- The TRG launcher, with Play, Display, Graphics, Gameplay, Online, Achievements, Controls and About pages
- Package installation from your own game, with version checks and **Reinstall from package...**
- Launcher self-update: the launcher checks this project's GitHub releases once per start, and nothing is installed until you choose **UPDATE NOW** and then **RESTART TO UPDATE**. Game files, `user_data`, logs and caches are kept.

**Play**
- Online multiplayer for up to four players
- Offline achievements, saved on your PC, in the launcher and in the game with F7
- The game's original languages: English, French, German, Italian and Spanish
- Português Brasil, an unofficial translation added by this port
- **Skip intro** goes straight to the title screen
- **Exit Game** closes the program cleanly
- Xbox controllers work without setup, and the keyboard works too

**Graphics**
- **Render Resolution** from 1x to 4x (1280×720 to 5120×2880)
- MSAA: 2x, or 4x in Compatibility mode
- Anti-aliasing: Off, FXAA or FXAA Extreme
- Texture filtering: the game's own, Off, or 2x to 16x anisotropic
- Windowed or Borderless Fullscreen, switched while playing with F11

**Overlays and diagnostics**
- F2 shows or hides the FPS display
- F3 shows or hides the Ping display while Online play is on
- **Detailed logging** is on by default, and **Open logs folder** is on the About page
- Crash reporting: after a crash, the session log ends with a `CRASH REPORT` section. It stays on your PC.
- Portable `user_data`: settings, saves, achievements and caches stay next to the program, so moving that folder moves everything

**Menus**
- Leaderboards, Xbox Live Party and Gamer Card entries are removed from the menus, because this port has no Xbox Live service

## Online multiplayer

Online play is implemented by this port and is not Xbox Live. Play with up to three friends through the game's own Multiplayer menus.

- Add **friends** by friend code on the launcher's Online page, after turning on Online play and running the Connection test.
- Send **invites** with **X Invite Friends** in your lobby. Invites you receive open in the game.
- Players connect **Direct** when they can, and through a **Relay** for any pair that cannot.

## Português Brasil

Português Brasil is an unofficial translation added by this PC port, for the game text and the launcher. It was not part of the original Xbox 360 release.

## Current limitations

- The game uses its original 16:9 presentation.
- Native ultrawide is not included in v0.1.0.
- Internal Render Resolution is not included in v0.1.0. **Render Resolution** 1x to 4x is the supported way to raise image quality.
- Linux Vulkan rendering remains experimental and has not yet been validated on real Linux hardware or Steam Deck.
- Two Direct3D 12 rendering fixes, for stale geometry and a cyan flicker on the turtles, do not yet have equivalent Vulkan fixes.
- Linux self-update needs the AppImage to keep its release file name.
- If a player closes the window during an online match, the others can wait up to 30 seconds before the match continues.
- The Windows binaries are not code-signed, so SmartScreen may show a warning the first time. If you downloaded the release from this page, choose **More info**, then **Run anyway**.

## Reporting problems

If the game does not start or crashes, please [open an issue](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues) and attach the newest `tmnt_turtles_in_time_reshelled_*.log` file from the `logs` folder next to the program, with a line about what you were doing.

## Requirements

- **Windows:** Windows 10 or 11 (64-bit), a DirectX 12 graphics card, and the latest [Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- **Linux (experimental):** x86_64 with glibc 2.31 or newer and a working Vulkan driver.
- **Both:** your own Turtles in Time Re-Shelled Xbox Live Arcade package, about 270 MB, and about 300 MB of free disk space next to the program for the game files.

## Checksums

The SHA-256 of each file is also published with the release in its `.sha256` file.

| File | SHA-256 |
| --- | --- |
| `TurtlesInTimeReshelled-v0.1.0-windows-x64.zip` | `3b8d511d7818fc5e1759aba7f7a99a97df73cca200d92bf1fc20a33499c2c274` |
| `TurtlesInTimeReshelledRecompiled-v0.1.0-x86_64.AppImage` | `51a2985b5a04b12278e1857ebc8977ac8c30270b4f57d93ea2e14e16255c7ccd` |

## VirusTotal

The final release artifacts and the main Windows programs were scanned with VirusTotal before publication and had 0 detections at release preparation time.

| File | Result | VirusTotal |
| --- | --- | --- |
| TurtlesInTimeReshelled-v0.1.0-windows-x64.zip | 0 detections | [VirusTotal report](https://www.virustotal.com/gui/file/3b8d511d7818fc5e1759aba7f7a99a97df73cca200d92bf1fc20a33499c2c274) |
| TurtlesInTimeReshelledRecompiled-v0.1.0-x86_64.AppImage | 0 detections | [VirusTotal report](https://www.virustotal.com/gui/file/51a2985b5a04b12278e1857ebc8977ac8c30270b4f57d93ea2e14e16255c7ccd) |
| Main Windows executable, tmnt_turtles_in_time_reshelled.exe | 0 detections | [VirusTotal report](https://www.virustotal.com/gui/file/f1a20a8e92b6bb29912129c3ec8410ac4dde699ff636c05186406c48c51ac488) |
| trg-updater.exe | 0 detections | [VirusTotal report](https://www.virustotal.com/gui/file/89afdc30b6e67f38adbb3caef4eedabf0558633fe718c9f68e82819b06f5e677) |

VirusTotal is an additional check, not a guarantee of safety, so please also verify the SHA-256 of your download.

## Legal

This is an unofficial fan-made PC port. It is not affiliated with or endorsed by the game's developer, publisher or rights holders. No copyrighted game files are included, and you must provide your own supported copy of the game.
