# Linux and AppImage manual checklist

Static parity is covered by `tools/test.ps1`, `tools/test.sh` and `tools/parity_gate.py`. The checks below need a real
Linux session. Nothing here is run automatically.

## Launcher

- [ ] The AppImage opens the launcher without a terminal error.
- [ ] The sidebar shows Play, Display, Graphics, Gameplay, Online, Achievements, Controls, Bug Report and About.
- [ ] Every page opens and every control is usable at 100%, 125% and 150% display scaling.
- [ ] Ctrl+S saves; Enter starts; Alt+F4 and the Quit button close the launcher.
- [ ] A controller navigates (D-pad/stick, A confirm, B back, Start play) and the keyboard does the same.
- [ ] Settings survive a restart; the launcher visibility setting and Shift recovery work.

## Game

- [ ] PLAY starts the game with the chosen window mode, resolution, VSync, filtering and anti-aliasing.
- [ ] The Graphics card list shows the Vulkan devices and the chosen one is used.
- [ ] F2 shows the frame counter, F3 shows Ping while Online play is on, F11 switches fullscreen, F7 shows achievements.
- [ ] Skip intro, language and the four local player names work.
- [ ] Player names, settings, saves, achievements and `user_data` stay outside the AppImage mount.
- [ ] A crash appends a `CRASH REPORT` block to the session log.

## Online

- [ ] Online play starts, the online name is saved and the friend code is shown and copied.
- [ ] The Connection test reaches the service and reports the network.
- [ ] Friends can be added, accepted, declined and removed; incoming and outgoing requests appear.
- [ ] The Lobby lists compatible players with **Players in Lobby (N)**, **Updated just now** and **Refresh lobby**; Show me in Lobby works, and an invite opens the game.
- [ ] A direct UDP match works; a relay match works when direct is blocked; Ping shows; reconnect works.

## Bug Report

- [ ] The Bug Report page lists logs and the preview is readable.
- [ ] A report is sent, shows a Report ID and appears in My reports.
- [ ] Refresh shows a developer reply and No longer available; incident.status/read never crash on malformed answers.

## Updates and shutdown

- [ ] The update check runs, a newer AppImage downloads, verifies and replaces the file on restart.
- [ ] Exit Game closes cleanly and exiting and relaunching keeps `game`, `logs` and `user_data` next to the AppImage.

## SNES, Classic Soundtrack and Cheats

These are integrated from the historical chain `01f0d7e..44bdc30` and registered on both platforms. The checks below
still need a real Linux run.

- [ ] F9 opens the SNES menu; Classic Soundtrack, Directional Combat, Throw Control, Disable Double Jump and the Dash
      Off/Auto/Manual choice work and persist.
- [ ] A USA, Europe and Australia ROM imports, validates, survives a restart and can be removed.
- [ ] Classic Soundtrack plays, switches back to the original music, follows the music volume, and its loudness
      correction sounds even.
- [ ] F10 opens Cheats; Infinite Health, Infinite Lives, Damage Multiplier, Turbo Attack, Turbo Strong Attack, Super
      Armor, Spin Attack and Unlock all stages work.
- [ ] Turning Online play on switches every saved cheat Off; the Cheats page and F10 show **Cheats are disabled while Online Play is enabled**, and turning Online play off restores no cheat.
- [ ] Two players negotiate SNES settings and per-seat gameplay preferences; a mismatch is reported and locks the
      session as designed.
