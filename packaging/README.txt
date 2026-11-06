Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled Recompiled
Version @VERSION@

Teenage Mutant Ninja Turtles: Turtles in Time Re-Shelled for Xbox Live Arcade,
statically recompiled to run natively on Windows with the ReXGlue SDK. This is
a native PC port, not an emulator.

Source code and issue tracker:
https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled


NO GAME FILES ARE INCLUDED
--------------------------
You need your own copy of the game: the Turtles in Time Re-Shelled Xbox Live Arcade
package. This port only runs this exact release:

  Title ID:      584109D2
  Game version:  0.0.1.3 (Xbox Live Arcade)

Any other version is refused with "Different version of the game".


REQUIREMENTS
------------
- Windows 10 or 11, 64-bit
- A DirectX 12 capable graphics card
- The latest Microsoft Visual C++ Redistributable 2015-2022 (x64). Most PCs
  have it. If Windows reports a missing VCRUNTIME140.dll or MSVCP140.dll,
  install it from https://aka.ms/vs/17/release/vc_redist.x64.exe
- Your own Turtles in Time Re-Shelled Xbox Live Arcade package (about 270 MB), plus
  about 300 MB of free disk space next to the program for the game files

Linux has its own download, an experimental AppImage. It has not yet been
validated on real hardware or on Steam Deck. Its instructions are on the
project page.


INSTALL
-------
1. Extract the ZIP anywhere you can write to (not "Program Files").
2. Run tmnt_turtles_in_time_reshelled.exe. The launcher opens.
3. On the Play page, click "Install from package...".
4. Select your Turtles in Time Re-Shelled Xbox Live Arcade package. It is one file
   with a long hexadecimal name and no extension. On an Xbox 360 hard drive it
   is in Content\0000000000000000\584109D2\000D0000\
5. Wait while the launcher checks the package and copies the game files into
   the "game" folder next to the program.
6. When the Play page says "Ready to play", press PLAY.

To repair the game files, use "Reinstall from package..." on the Play page.
Your settings and saves are kept.


UPDATES
-------
The launcher checks the project's GitHub releases once per start. When a newer
release exists, an update banner appears in the header; nothing is installed
until you choose UPDATE NOW and then RESTART TO UPDATE. The update replaces
only the port's own program files. Game files, user_data, saves, settings,
logs and caches are kept.


WINDOWS SMARTSCREEN
-------------------
The first time you run tmnt_turtles_in_time_reshelled.exe, Windows SmartScreen may show a
warning, because the program is not digitally signed. If you downloaded the
release from the project's GitHub releases page, choose "More info", then
"Run anyway".


PLAYING
-------
- F2 shows or hides the FPS counter.
- F3 shows or hides Ping while Online play is on.
- F7 opens the achievements.
- F11 switches between Windowed and Borderless Fullscreen.
- Xbox controllers work without setup.
- The keyboard works too; the Controls page lists the keys.
- To open the launcher again after turning it off, hold Shift while starting.

The launcher's Gameplay page has the Language (including the port's unofficial
Portugues Brasil translation) and Skip intro. The Graphics page has the Render
Resolution, from 1x to 4x, anti-aliasing and texture filtering. The picture is
16:9.


ONLINE
------
Online play is with friends, through the game's own Multiplayer menus. On the
launcher's Online page, turn on Online play, run the Connection test and add
friends by friend code. Players connect directly when they can, and through a
relay when they cannot. Invites you receive open in the game.


WHERE THINGS ARE
----------------
- Game files:     the "game" folder next to tmnt_turtles_in_time_reshelled.exe
- Logs:           the "logs" folder next to tmnt_turtles_in_time_reshelled.exe
                  (the launcher's About page has an Open logs folder button).
                  Detailed logging, also on the About page, is on by default.
                  Turned off, the log keeps only what identifies the session
                  and what went wrong.
- Settings, saves, achievements and shader cache:
                  the "user_data" folder next to tmnt_turtles_in_time_reshelled.exe

If the game does not start or crashes, please attach the newest file from the
"logs" folder to a report at
https://github.com/saulob/tmnt-turtles-in-time-reshelled-recompiled/issues


LICENSE
-------
The original project code of this port is licensed under the GNU General
Public License v2.0 only (GPL-2.0-only). The full text is in LICENSE.

Third-party components keep their own licenses. Their license files and
notices are in the "licenses" folder: ReXGlue-LICENSE.txt and
THIRD-PARTY-LICENSES.txt.

Game files are not included and are not covered by this project's license.
You must own the game to use this port. This port is not affiliated with or
endorsed by the makers or publishers of Turtles in Time Re-Shelled.
