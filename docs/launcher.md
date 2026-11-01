# Launcher

The pre-game launcher is [TRG Launcher](https://github.com/TekRantGaming/trg-launcher), integrated as in Hard Corps: Uprising Recompiled, the template this project was bootstrapped from.

## Architecture

- Embedded, not standalone. TRG draws inside ReXGlue's own Dear ImGui frame, in the game window. There is no second window, process, SDL or ImGui.
- Dear ImGui is the copy inside `rexruntime` (1.92.5). CMake passes it to TRG as `TRG_LAUNCHER_IMGUI_TARGET`, with `TRG_LAUNCHER_STANDALONE`, `TRG_LAUNCHER_FETCH_DEPS` and `TRG_LAUNCHER_BUILD_DEMO` off, so nothing is downloaded.
- Lifecycle, from `ReXApp`:
  1. `SetupEnvironment` loads `tmnt_turtles_in_time_reshelled.toml`; `OnPostInitLogging` loads the player's `settings.toml` over it. The Xenos GPU plugin is not loaded here. ReXGlue loads it in `SetupPresentation`, and the Graphics cvars it registers take the player's values from the file.
  2. `OnPreSetup` records the saved game settings and decides whether the launcher shows. If it does, `PrepareLauncherWindow` sets the window for the launcher. Then the window settings this process creates its window with are recorded. Graphics keys are not registered yet, so they are recorded as unregistered.
  3. `SetupPresentation` creates the window and loads the GPU plugin. `OnFinalizePaths` calls `CaptureLateRegistered`, which takes the Graphics values now that they are registered, then shows the launcher and returns `nullopt`; the runtime is not built yet. A key the player has already edited in this process is never overwritten.
  4. **Play** (mouse, Enter or Xbox Start) calls `RequestPlay`, which is the only save: if it fails, the launcher stays open with "Could not save the settings." and nothing starts. On success it shows "Settings saved. Starting game..." at full brightness for 0.4 s, timed with ImGui and without sleeping. The launcher's controls are dimmed and disabled during that time, and its keyboard, gamepad and mouse input is released each frame, so Play, Save, Ctrl+S, Esc and the D-pad do nothing. Then, if the window size, monitor, VSync or graphics card differs from this process's window, the game is relaunched once and this process quits (`StartGame` does not save). Otherwise the pending settings are applied and the runtime starts.
  5. **Relaunched copy**: `--tmnt_skip_launcher=true` skips the launcher, and the window is created natively with the saved settings.

## Settings

| File | Written by | Role |
| --- | --- | --- |
| `<exe dir>/tmnt_turtles_in_time_reshelled.toml` | the build (copied from `config/`) | shipped defaults, never written by the launcher |
| `<user data>/settings.toml` | the launcher | the player's choices, loaded last |

- Normally user data is the `user_data` folder next to the program ([user-data.md](user-data.md)). `tools\build.ps1 -Run` passes `--user_data_root`, which wins, so user data is `build\user_data` (kept by `-Clean`) and nothing migrates.
- Launcher edits are pending in `tmnt::GameSettings` until Save or Play. Save writes only the changed keys to `settings.toml`, through `trg::TextFileSettings`; comments and other lines are kept. A key whose cvar is not registered is refused rather than written.
- Play is blocked, and does not save, while the game files are missing or invalid or an install is running.
- Reset all settings puts the launcher's values back to the shipped ones, pending until Save or Play.
- Legacy repair: an unreleased development build wrote `key = on` and `key = off`, which TOML rejects and which made ReXGlue ignore the whole file. Before the file is parsed, each exact bare `on`/`off` value is rewritten as `true`/`false`. Nothing else changes. The original is kept as `settings.toml.bak`. Once the file holds no bare on/off, this does nothing.
- The dependency is the `third_party/trg-launcher` submodule, pinned to `b384a28` (TRG Launcher 1.1.0). After cloning, run `git submodule update --init third_party/trg-launcher`. The submodule is never modified: CMake copies the clean checkout to `<build>/trg-launcher-patched` at configure time, applies the patches in `patches/trg-launcher` there (hyperlinks, download headers, the release download address allow-list with checked redirects, the Linux file picker fallbacks: portal, zenity, kdialog, and Ctrl+S in text inputs; applied with `git apply --ignore-whitespace`, so the checkout's line endings do not matter), and compiles that copy. It fails when the submodule is at another revision or carries local changes. When the submodule is not checked out, CMake falls back to the checkout named by `TMNT_TRG_LAUNCHER_DIR` (CMake cache or environment), copies it the same way and applies the patches without modifying it. The upstream repository has no LICENSE file.

## Window

The launcher window and the game window are separate.

- **Launcher window:** Windowed, 1280×720 logical (capped at 90% of the primary display's work area), centred on the primary display by SDL (`monitor = 0`).
- **Game window:** created by the game process with the saved Window size, Monitor, VSync and Display mode.

| Setting | When it takes effect |
| --- | --- |
| Window size, Monitor, VSync, Graphics card | Play. If any differs from the launcher's window, the game restarts once in a new window. |
| Display mode | Play. In the launcher's window when the window already matches, otherwise in the new window. |
| Aspect Ratio, graphics, frame counter, keyboard | Play, in the same window |

### Window position

The launcher reopens at the position and size it was closed with, and maximized if it was maximized. A maximized launcher keeps its previous normal size for when it is restored. The values are saved a moment after the window settles and again when the launcher closes, as `tmnt_launcher_x`, `tmnt_launcher_y`, `tmnt_launcher_width`, `tmnt_launcher_height` and `tmnt_launcher_maximized` in `settings.toml` ([user-data.md](user-data.md)). Reset to defaults leaves them alone.

If less than a small corner of the saved window lies on a connected display (for example a monitor was unplugged), the launcher opens in its default place instead; a saved maximized state is still applied. Displays left of or above the primary one are handled. On Windows the window is placed before it is first shown; on Linux right after it opens, and the desktop may ignore the position. **F11** in the launcher switches the launcher itself to borderless fullscreen and back, to the same window or maximized state. If the launcher is closed while in that fullscreen it reopens in it (`tmnt_launcher_fullscreen`), and F11 returns to the saved window or maximized state. Playing leaves it for the game without changing that choice; only F11 does. The game window is not affected: it opens as the Window size and Monitor settings say, and Display mode is not part of the saved position.

Why a restart: ReXGlue 0.10.0 has no API to resize or move an open window (its `window.h` lists resize as not implemented). The window's size and display are fixed when it is created. A window created with the game's size and display keeps the right Windowed size and position for F11.

- Default game window: 853×480 logical, from `config/tmnt_turtles_in_time_reshelled.toml`. A value in `settings.toml` replaces it.
- With the defaults, a normal Play from the launcher relaunches the game once. The launcher and the game have independent window sizes, so this is expected, not an error. The relaunched copy opens its window at the game size directly.
- Display mode is switched in place only when the window already matches the game's size, monitor and VSync. Otherwise it is applied by the relaunched copy, so the Windowed size F11 restores is always the game's.
- The launcher's 1280×720 target is capped to the usable desktop, so on a display where it does not fit (for example 2560×1440 at 225%, where it becomes 1024×576) the launcher is smaller.
- Sizes that cannot fit the current display at its scaling are hidden from the Window size list, except the current one.
- **Aspect Ratio** (Display page, after Monitor) is the game's target rendering format, in one dropdown: 16:9 Original (default), 16:10, 21:9 Ultrawide, 32:9 Super Ultrawide, 4:3 and 5:4. It applies when Play starts the game. The Window size list shows the sizes of the chosen format; changing the format moves Window size to that format's default unless the current size already belongs to it. The value is saved as `tmnt_aspect_ratio` in `settings.toml`; a file without it means 16:9 Original.
- A `present_letterbox` line from an older `settings.toml` is removed when the game starts. The presenter always letterboxes the chosen format, so there is no Stretch mode.

### Aspect Ratio and the guest picture

The Aspect Ratio control is inherited from the Hard Corps launcher. Here it sets only the generic part: the guest video mode (`video_mode_width` at 480 lines, for example 853×480 at 16:9) and the presenter letterbox. The native-aspect hooks that made the Hard Corps engine render a wider canvas are game-specific and were not carried over, so how Turtles in Time Re-Shelled responds to a non-16:9 video mode is not verified. 16:9 Original is the default and the only format expected to look right.

The Render resolution labels assume a 1280×720 render base at 16:9, as in the template. The real Turtles render size has not been measured yet.

### Relaunch rule and loop prevention

- The comparison is between the pending settings and the window this process created. The launcher's own window is what this process created, so it counts as a mismatch when the game's settings differ from it.
- A relaunch starts a copy with `--tmnt_skip_launcher=true`. That copy never shows the launcher, so it never compares or relaunches.
- Launcher hidden: the game starts directly with the saved settings.
- Shift at start shows the launcher with its own window. The relaunched copy skips it even if Shift is still held.

### Monitor numbering

- ReXGlue's `monitor = N` opens the window on `SDL_GetDisplays()[N-1]`. SDL lists the primary display first, then the others in Windows order.
- The launcher's Monitor list uses the same order (`ListMonitors` in `src/platform.cpp`). The order was checked against the SDL 3.4.10 source; the SDK ships SDL 3.5.0, whose source is not on disk.
- At startup the game logs a warning when the window did not open on the display the setting names.
- Monitor `0` (Default) is the primary display.

## Game files and install

- **Game folder:** `--game_data_root`, else `game` next to the program (`TurtlesInTimeReshelledApp::OnConfigurePaths`). Development builds pass `game\extracted`. The package's root is extracted there: `default.xex`, `TMNT_r.xex`, `Data\` (with `Data\TMNT.bf`, `Data\Cache` and `Data\shaders`), `ArcadeInfo.xml`, the achievement images, `banner.png` and `title_icon.png`. Settings, saves and the shader cache are in the user data folder (`user_data` next to the program, or `--user_data_root`); logs are in `logs` next to the program.
- **One readiness check:** `CheckGameFiles` (`src/game_files.cpp`) is used by the Play page, the install verification and the clean-start decision. Ready means `default.xex` is the supported build (4,808,704 bytes, CRC32 `FC04059D`), `TMNT_r.xex` is the supported module (2,002,944 bytes, CRC32 `2D0061F9`) and `Data` exists. Missing and unsupported are separate states. See [package-analysis.md](package-analysis.md) for the SHA-256 values.
- **Launcher visibility:** `LauncherWanted` (`src/launcher.h`) shows the launcher whenever the game files are not ready, whatever `tmnt_launcher` or `--tmnt_skip_launcher` say, so turning the launcher off cannot hide the only way to install the game. A relaunch after PLAY still skips it, because the files are ready then.
- **Install from package** (`InstallFromPackage`, on a TRG `Task`, so the UI stays responsive): reads the STFS header and requires Title ID `584109D2` and content type `000D0000`, reads and validates the file table, checks free space, extracts into `game-install-temp` next to the game folder, runs `CheckGameFiles` on it, and only then swaps it in (`game` becomes `game-install-backup`, the staging folder becomes `game`, the backup is deleted). A wrong package, damaged package, bad executable, write error or Cancel leaves the existing game untouched and removes the staging folder. A folder that holds other files is never replaced.
- **DLC install and removal** (`InstallDlcFromPackage`, `ListInstalledDlc`, `RemoveInstalledDlc`): the generic Marketplace-content installer from the template is kept, keyed by Title ID `584109D2` and content type `00000002`, in the SDK's layout `<user data>\0000000000000000\584109D2\00000002\<content ID>\` with `Headers\00000002\<content ID>.header` beside it. No DLC package for this title is known, so the DLC page is not registered (`kShowDlcPage` in `src/launcher.cpp`). When enabled it lists verified installed packages by their display name, with Install DLC... and Remove.
- **Package reader** (`src/stfs.cpp`): read-only STFS (`LIVE`, `PIRS`, `CON `) with consecutive files, which is what Xbox Live Arcade packages are. The supplied Turtles package uses this layout (no fragmented entries). Every entry name is checked before anything is written: one printable ASCII path component, no `..`, separators, drive or stream markers, device names, or trailing dot or space; parents must be directories and nesting is limited. Extraction also checks that each destination stays inside the staging folder. Block hashes and the Microsoft signature are not verified.
- **Tests:** `cmake --build build --target tmnt_installer_tests` builds `tests/installer_tests.cpp`. It builds synthetic packages (with a `default.xex` and a `TMNT_r.xex` of the supported sizes and CRC32s made from generated bytes), so it needs no game files. `tmnt_installer_tests --package <file> --reference <extracted folder>` also extracts a real package once into a temporary folder and compares it.

## Pages

| Page | Setting | Cvar |
| --- | --- | --- |
| Play | Install / Reinstall from package | (game files, see below) |
| | Show this launcher | `tmnt_launcher` |
| Display | Display mode | `fullscreen` |
| | Window size | `window_width`, `window_height` |
| | Monitor | `monitor` |
| | VSync | `d3d12_allow_variable_refresh_rate_and_tearing` (inverted) |
| | Aspect Ratio | `tmnt_aspect_ratio` |
| Graphics | Graphics card (Windows) | `tmnt_graphics_adapter` |
| | Render resolution | `resolution_scale` |
| | Internal render resolution | `tmnt_internal_render_scale` |
| | Anti-aliasing | `swap_post_effect` |
| | Multisampling | `tmnt_msaa_compatibility` |
| | Texture filtering | `anisotropic_override` |
| Gameplay | Language | `tmnt_language` |
| | Frame rate in title | `tmnt_show_fps` |
| | Render resolution in title | `tmnt_show_render_resolution` |
| | Frame counter | `tmnt_show_frame_counter` |
| | Player 1 - Player 4 | `tmnt_player_1_name` .. `tmnt_player_4_name` |
| Online | Online play | `tmnt_online_enabled` |
| | Online name | `tmnt_online_name` |
| | Connection test | `tmnt_online_connection_test` (the finished test and the configuration it ran for) |
| | Ping display | `tmnt_online_ping_display` |
| Controls | Keyboard | `mnk_mode` |
| | Keyboard controls | `keybind_*` (one per action), reset by Keyboard defaults |
| About | Detailed logging | `detailed_logging` |

### Graphics

| Control | Cvar | Values (default) |
| --- | --- | --- |
| Graphics card (Windows) | `tmnt_graphics_adapter`, sets `d3d12_adapter` | the DXGI adapter index; the first detected adapter when unset or not present (`0`) |
| Render resolution | `resolution_scale` (integer 1–8, the launcher offers 1–4) | `1` Original, `2`, `3`, `4` (`1`) |
| Internal render resolution | `tmnt_internal_render_scale` (percent of the render resolution) | `100` 100% Native, `75` 75% Performance, `50` 50% Low-end (`100`) |
| Anti-aliasing | `swap_post_effect` | `none` Off, `fxaa`, `fxaa_extreme` (`none`) |
| Multisampling | `tmnt_msaa_compatibility` | `false` 2x MSAA, `true` 4x MSAA (Compatibility) (`false`) |
| Texture filtering | `anisotropic_override` | `-1` Game (no override), `0` Off, `2` 2x, `3` 4x, `4` 8x, `5` 16x (`3`, 4x) |

Multisampling only decides how the render targets the game itself draws with 2x MSAA are drawn on the host: native 2x MSAA, or 4x MSAA using two of its samples, for GPUs or drivers whose 2x MSAA misrenders. It cannot turn the game's multisampling off, and the compatibility choice costs more GPU time. The game sets the Xenos `native_2x_msaa` from it at every start, also replacing a `native_2x_msaa = false` that older launchers wrote for "Off".

Shader compilation is not a launcher setting; the game uses ReXGlue's default, `async_shader_compilation = true`.

- **Graphics card** lists the hardware GPUs Windows reports that can create a Direct3D 12 device, by the names DXGI gives them; software adapters are not listed. It is shown on Windows only.
  - Its DXGI index, counted as ReXGlue counts for `d3d12_adapter` (`EnumAdapters1` order), is passed as `d3d12_adapter`. A missing key, the retired `auto` or a GPU that is no longer present selects the first listed GPU.
  - The graphics device is created before the launcher shows, so a change restarts the game once on Play, like a window change.
  - The log shows the choice (`Graphics card: <name> (adapter N)`) and, from ReXGlue, the GPU actually used (`DXGI adapter: <name>`).
  - Linux has no such row: `vulkan_device` indexes the instance ReXGlue creates, which the launcher cannot reproduce reliably, and Vulkan keeps its automatic discrete-GPU preference.
- **Render resolution** labels show the presented output at each scale for the chosen Aspect Ratio: at 16:9, Original 1280×720, 2x 2560×1440, 3x 3840×2160, 4x 5120×2880. They do not depend on Window size. 2x is a natural match for 1440p displays. The recommendation is informational only, and the default stays Original. Higher scales cost more GPU time.
- The game takes the render resolution when its GPU starts, after Play, so a change applies to the game that Play starts. The GPU driver can lower a scale it cannot support; the game logs a warning when it does.
- Render resolution is not a relaunch trigger. It never changes the window.

Internal render resolution is the resolution the GPU draws the game at before the picture is scaled to the selected Render resolution. Render resolution keeps selecting the target; this setting only reduces the work behind it, and says nothing about the window.

- **100% Native** is the current behavior, unchanged: the game is drawn at the Render resolution. A missing or unknown value is 100%.
- **75% Performance** is the recommended choice for a GPU that is too slow at the Render resolution. **50% Low-end** is meant for low-end hardware. Lower values are less sharp. No frame rate gain is claimed; measure it on the GPU in question.
- The internal size is the Render resolution times the percentage, in the chosen Aspect Ratio's render base, so the ratio is kept: at 16:9, 1280×720 at 75% is 960×540, at 50% 640×360; 2x is 1920×1080 and 1280×720; 3x is 2880×1620 and 1920×1080; 4x is 3840×2160 and 2560×1440; at 21:9 the 1680×720 base gives 1260×540 at 75% and 840×360 at 50%. The game logs one line with it at start, for example `Internal render resolution: 75% 960x540 -> 1280x720`.
- On Windows a value below 100% also makes the D3D12 render target path the host one (`render_target_path_d3d12 = rtv`) when that setting is empty, which replaces the rasterizer-ordered path Intel GPUs use by default; the game logs `Internal render resolution: using D3D12 host render targets`. 100% leaves the path selection alone, and a `render_target_path_d3d12` set by hand is never overridden.
- Like Render resolution, the game takes it when its GPU starts, after Play; it is not a relaunch trigger and never changes the window, the guest video mode, the aspect ratio or VSync.
- It is a separate setting from `resolution_scale`; Texture filtering, Anti-aliasing and Multisampling are independent of it. The runtime patches that implement it (and the cases where it draws at the full Render resolution instead) are in [rexglue-runtime-patch.md](rexglue-runtime-patch.md#why-0036-0037-and-0038).

- Every default is the value the game already used.
- VSync controls tearing only. The D3D12 presenter always presents with sync interval 0; frame pacing comes from the guest `vsync` cvar, which the launcher does not touch.
- Keyboard gameplay input can be turned on or off, and the bindings edited, on the Controls page (see below).
- Any controller SDL recognizes drives the launcher (a Steam Controller, a DualSense or an Xbox pad): Start plays; A, B, the D-pad and the left stick navigate. ReXGlue's input system only starts with the game.
- The accent is a neon green (`kAccent`) with a dark emerald for the ready card, both in `src/launcher.cpp` with the header gradient. Amber is kept only for the "game files" warning, which is a status colour.

### Gameplay

| Control | Cvar | Values (default) |
| --- | --- | --- |
| Language | `tmnt_language` | `en` English, `fr` French, `de` German, `it` Italian, `es` Spanish (`en`) |
| Frame rate in title | `tmnt_show_fps` | `false` Off, `true` On (`true`) |
| Render resolution in title | `tmnt_show_render_resolution` | `false` Off, `true` On (`false`) |
| Frame counter | `tmnt_show_frame_counter` | `false` Hidden, `true` Shown (`false`) |
| Player 1 - Player 4 | `tmnt_player_1_name` .. `tmnt_player_4_name` | up to 15 characters and 15 UTF-8 bytes (`Player 1` .. `Player 4`) |

- **Language** is the only language setting the game has: a single text language for menus, the HUD and movie subtitles. There is no separate voice language, because the game has no spoken dialogue for any language — the archive (`Data\TMNT.bf`) has no audio files at all, and its seven Bink movies share one audio track with only their subtitles translated per language. The five options are exactly the languages with text banks in the archive (`00_French`, `01_English`, `05_German`, `06_Italian`, `07_Spanish` `UI_TextBank`/`ErrorTextBank`/`WaitingTextBank`/`MovieSubtitleBank`); no other language has banks. It is applied by setting ReXGlue's `user_language` to the Xbox `XLanguage` ID the game itself maps to that bank (English 1, French 4, German 3, Italian 6, Spanish 5) before the game boots: `default.xex` reads `user_language` once at startup via `ExGetXConfigSetting(XCONFIG_USER_LANGUAGE)` and switches its text-bank index from it, with no other runtime re-selection. An unknown or missing saved value falls back to English. The same `user_language` cvar already drives achievement name and description localization (`src/achievement_alias.cpp`), so no separate state was introduced.
- **Local player names** change only the name the game shows for local players 1 to 4; an invalid saved name falls back to the default. The XUID and the profile and save directory never change. A name left at its default keeps the game's own name for that user.

### Online

Online play (`tmnt_online_enabled`, `false` Off by default), Online name (`tmnt_online_name`, up to 16 letters, digits, `-` or `_`),
Ping display (`tmnt_online_ping_display`, `false` Hidden by default, F10 in game) and the stored Connection test result
(`tmnt_online_connection_test`, written by the launcher, not meant to be edited). With Online play on, PLAY waits for a
Connection test whose result is Overall usable for the current UDP configuration. Friends, invites and the four-player
party are on the same page; see `docs/online.md`.

### Page order

The sidebar order is Play, Display, Graphics, Gameplay, Online, Achievements, Controls, About (DLC after Play when `kShowDlcPage` is on). Achievements is read-only: it lists the achievements ReXGlue reads from the game's XDBF and which are unlocked. The package's `ArcadeInfo.xml` names 12 achievements with IDs 1 to 12.

The Hard Corps launcher also had Cheats, Skip intro and a Frame rate choice with visual interpolation. Those were implemented with hooks into the Hard Corps engine and are not part of this project.

### Controls

The Controls page opens with **Keyboard**, which turns ReXGlue's keyboard-to-pad emulation on or off; it is on by default. The controller works either way, and the port's own hotkeys (F2 frame counter, F3 and F4 overlays, F7 achievements, F11 fullscreen, the console) do not depend on it.

Below it, one row per Xbox pad action: A, B, X, Y, the bumpers, the triggers, the sticks and their presses, the D-pad, and View / Menu. Guide is left out, as the pad ignores it. Click a key, then press the new one; Esc cancels. The launcher captures one key per action: a held Shift, Ctrl or Alt is not recorded as a modifier, so a binding such as the default `Shift+Up` can be restored only with Keyboard defaults. A stored binding with modifiers is shown and works in the game.

- While Keyboard is Off the rows are greyed out, but the mappings are kept and come back when it is turned on again.
- The bindings are ordinary launcher settings: saved with Save or Play, and put back by Reset all settings. **Keyboard defaults** restores exactly ReXGlue's pinned defaults, including the alternatives some of them use (`Semicolon,Space` for A, `X,Return` for Menu), and does not change the Keyboard On/Off choice. Capturing a key replaces the whole binding with that one key, so the alternatives are dropped on a rebind.
- They are stored as ReXGlue `keybind_*` cvars, so the same settings file also sets them when the launcher is skipped.
- Keys the port already uses are refused with a message: Escape, F2, F3, F4, F7, F10, F11, F12 and the backtick console key.

### About

The About page ends with a **Diagnostics** section. The launcher and the game share one `tmnt_turtles_in_time_reshelled_NNN.log` for the whole application session, including the relaunch that creates the game window. When the game is about to run, the log receives a compact `Runtime configuration` block: the Launcher Display, Gameplay and Graphics choices as the player made them, followed by the effective runtime OS, CPU, GPU, display, game identity, timing, renderer and input state. Selected values stay selected: `Graphics card` keeps the chosen detected GPU while `Runtime GPU` reports the adapter that actually resolved, and the selected window size stays distinct from the fitted window and drawable size. The block contains no user, repository or installation paths. **Detailed logging** is on by default: the full runtime log is kept after every session. Turned off, the run still writes a shorter log while it runs — warnings, errors and the startup lines that identify the port, platform, GPU, render settings and runtime configuration — but a clean exit discards that session's log, so normal sessions do not accumulate files. With Detailed logging off, closing the window removes the log like any other normal exit, unless it holds a crash report or was handed to a relaunched process: the process ends without shutting down its logging, because game threads may still write to it, so the file sink is closed and the file deleted at once. If Windows cannot delete it, a `.discard` marker is left and the next start removes the log. The choice in effect at exit decides whether the log is kept. If the game crashes, the log is kept and contains that context plus one `CRASH REPORT` block with uptime, exception details, native frames and any resolvable guest function; no minidump or second diagnostic file is written. Logging and crash reporting are never disabled. **Open logs folder** opens the folder that holds these logs. If a crash occurs, send the newest `tmnt_turtles_in_time_reshelled_*.log`; for a non-crash problem, turn Detailed logging on, reproduce it, and send the newest log.

## Not offered yet

| Option | Reason |
| --- | --- |
| Frame rate choice, cheats, skip intro | Hard Corps engine hooks; Turtles needs its own analysis first. |
| Deadzone, vibration, stick inversion, button remap | Would need Turtles-specific guest hooks. |
| Present effect (FSR/CAS) | Not exposed by the template either. Needs a restart. |
