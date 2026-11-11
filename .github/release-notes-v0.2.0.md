## TMNT Turtles in Time Re-Shelled Recompiled v0.2.0

An unofficial native PC port of *Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled* for Xbox Live Arcade. The game's own Xbox 360 code is statically recompiled to native PC code with the ReXGlue SDK, so this is a native program and not an emulator.

**No game files are included.** You need your own Xbox 360 copy of the game, in the exact version listed below. The new SNES features need your own SNES ROM; no ROM or SNES music is included either.

## Downloads

| Platform | File |
| --- | --- |
| Windows x64 | `TurtlesInTimeReshelled-v0.2.0-windows-x64.zip` |
| Windows x64 SHA-256 | `TurtlesInTimeReshelled-v0.2.0-windows-x64.sha256` |
| Linux x86_64 (experimental) | `TurtlesInTimeReshelledRecompiled-v0.2.0-x86_64.AppImage` |
| Linux x86_64 SHA-256 | `TurtlesInTimeReshelledRecompiled-v0.2.0-x86_64.sha256` |

Already on v0.1.0? The launcher offers this update on its own: choose **UPDATE NOW**, then **RESTART TO UPDATE**. Game files, `user_data`, saves, settings and logs are kept.

## Highlights

- **Classic Soundtrack**: play with the SNES *Turtles in Time* music, read from your own SNES ROM.
- **SNES Gameplay**: Directional Combat, SNES Throw Control, Disable Double Jump and SNES Dash, offline and online.
- **Cheats** for offline play, with an in-game **F10** menu.
- **Friend requests and a public Lobby** on the launcher's Online page.
- **Bug Report**: send a description and one sanitized log from the launcher, and follow replies in **My reports**.
- **Shredder lighting flicker fixed** on Windows.

## SNES Classic Soundtrack

- On the launcher's **Gameplay** page, **Import SNES soundtrack** reads your own ROM: `.sfc`, `.smc`, or a `.zip` holding one. The USA, Europe and Australia releases are recognised on their own. Other files are refused with the reason (invalid ROM, another game, unsupported soundtrack).
- **Classic soundtrack** On replaces the Re-Shelled music with the SNES music. Sound effects and voices are unchanged, and the game's Music volume still applies, mute included.
- The SNES music is raised by about 3.5 dB so both soundtracks play at a similar level, with a soft limit instead of clipping.
- **F9** switches it for the current session. Online, each player hears the soundtrack they chose.
- **Remove SNES soundtrack** deletes the imported music. Your ROM file is never changed, and it is not needed after the import.
- Windows and Linux.

## SNES Gameplay

- A new **SNES** page, also reachable in game with **F9**: **Directional Combat**, **SNES Throw Control**, **Disable Double Jump** and **SNES Dash** (**Off**, **Auto** or **Manual**). It needs a Ready SNES import. F9 changes last for the session only.
- Online, a mechanic is available only when every player's PC supports it with the same data. Each player's choice then applies to their own turtle, fixed for the match. PAL and NTSC imports share Directional Combat and Disable Double Jump; Throw Control and Dash are unavailable when they are mixed. Without an agreement, every mechanic stays Off and the match plays normally.
- Windows and Linux.

## Cheats

- A new **Cheats** page: **Infinite Health**, **Infinite Lives**, **Damage Multiplier** (Off, 2x, 4x), **Turbo Attack**, **Turbo Strong Attack**, **Super Armor** and **Spin Attack**. **F10** changes them for the session only.
- **Unlock all stages** on the Gameplay page makes every Quick Play stage selectable without changing your saved progress.
- Cheats are for offline play. Turning **Online play** on switches every saved cheat Off, and turning it off again restores nothing. The Cheats page and F10 say so while Online play is on, and Unlock all stages is not applied in an online session.
- Windows and Linux.

## Online, Friends and the public Lobby

- The Online page now has three tabs: **Settings**, **Friends** and **Lobby**.
- Friends are added by request. Send one by friend code, or from the Lobby. The other player sees it as **Pending** and can **Accept** or **Decline** it.
- **Show me in Lobby** lists you for other players with your online name and game version. The default name `Player` is shown as `Player-XXXX`. Your friend code is not shown in the Lobby. **Refresh lobby** and an "Updated" time show how fresh the list is.
- The online protocol is unchanged.
- Windows and Linux.

## Bug Report

- A new **Bug Report** page sends a short description and one game log you choose. You see a preview of exactly what will be sent before anything leaves your PC, and personal folders and names are removed from the log.
- Each report gets a report ID (`TMNT-XXXXXX`). **My reports** shows its status, the version where it was fixed, and any message from the developer.
- Reports carry the graphics diagnostics described below even with **Detailed logging** Off.
- Windows and Linux.

## Launcher

- New pages: SNES, Cheats and Bug Report. New page icons, drawn sharp at any Windows display scale.
- **Ctrl+S** saves from any page, and the footer lists Play, Quit and Save.
- Achievements are shown in the language you chose.
- Windows: the launcher no longer hangs while idle with a controller connected. It now reads controllers through XInput.
- Português Brasil covers the new pages.

## Graphics

- **Fixed (Windows):** in the final fight, Shredder no longer flickers to an almost black silhouette.
- **Linux:** the cyan turtle flicker fix that Windows got in v0.1.0 is now in the Vulkan renderer too. It is not yet confirmed on a Steam Deck.

## Diagnostics

v0.2.0 writes extra graphics diagnostics to the game log, so that a Bug Report can help with the two open graphics issues below. They are logged as warnings, so they stay in the log even with Detailed logging Off. They do not change what is drawn.

- Windows (Direct3D 12): lighting, render target, resolve and shader-compilation state for [issue #2](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues/2).
- Linux (Vulkan): skipped draws, stale vertex data, the turtle flicker fix and video textures for [issue #3](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues/3).

If you see either problem, please send a **Bug Report** right after it happens.

## Known issues

- **[#2](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues/2) Graphics bug in the first stage** (odd light and shadow on the characters, seen on an RTX 5070): not fixed yet. v0.2.0 adds diagnostics for it.
- **[#3](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues/3) Steam Deck graphical bugs**: not fixed yet. v0.2.0 adds the Vulkan turtle flicker fix and diagnostics for it.
- The Shredder flicker fix and the stale geometry fix are Direct3D 12 only; Linux does not have them yet.
- The game uses its original 16:9 presentation.
- Linux self-update needs the AppImage to keep its release file name.
- If a player closes the window during an online match, the others can wait up to 30 seconds before the match continues.
- The Windows binaries are not code-signed, so SmartScreen may show a warning the first time. If you downloaded the release from this page, choose **More info**, then **Run anyway**.

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

## Linux AppImage (experimental)

It needs x86_64 Linux, glibc 2.31 or newer and a working Vulkan driver. Run `chmod +x` on the AppImage and start it from a folder you can write to. This is a native Linux build, not Proton or Wine. `game`, `logs` and `user_data` are created next to the AppImage file. The [Linux guide](https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/blob/v0.2.0/docs/linux.md) covers requirements, Steam Deck setup and the fallback for systems where FUSE cannot mount the AppImage.

## Requirements

- **Windows:** Windows 10 or 11 (64-bit), a DirectX 12 graphics card, and the latest [Microsoft Visual C++ 2015-2022 Redistributable (x64)](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- **Linux (experimental):** x86_64 with glibc 2.31 or newer and a working Vulkan driver.
- **Both:** your own Turtles in Time Re-Shelled Xbox Live Arcade package, about 270 MB, and about 300 MB of free disk space next to the program for the game files. For the SNES features, your own SNES *Teenage Mutant Ninja Turtles IV: Turtles in Time* ROM.

## Checksums

The SHA-256 of each file is also published with the release in its `.sha256` file.

| File | SHA-256 |
| --- | --- |
| `TurtlesInTimeReshelled-v0.2.0-windows-x64.zip` | `pending` |
| `TurtlesInTimeReshelledRecompiled-v0.2.0-x86_64.AppImage` | `pending` |

## Legal

This is an unofficial fan-made PC port. It is not affiliated with or endorsed by the game's developer, publisher or rights holders. No copyrighted game files or SNES data are included, and you must provide your own supported copies of the games.
