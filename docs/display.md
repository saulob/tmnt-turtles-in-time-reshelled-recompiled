# Display

## Window

- Default game window: windowed, 853x480 logical, centred on monitor 1 (`config/tmnt_turtles_in_time_reshelled.toml`). A user's `settings.toml` value replaces it.
- The launcher window is separate: it opens windowed, 1280x720 logical capped to the desktop, on the primary display, whatever the saved game settings are ([launcher.md](launcher.md)). It reopens where it was last left ([launcher.md](launcher.md#window-position)).
- Play starts the game with the saved Window size, Monitor, VSync and Display mode. A new window is needed when the size, monitor or VSync differs from the launcher's, which relaunches the game once. The window is never moved or resized in place: ReXGlue 0.10.0 has no such API ([launcher.md](launcher.md)).
- Guest video mode: 480 lines at the Aspect Ratio's width (853x480 at 16:9), set when the game starts and independent of the window size. Window size is the host window only; the guest picture is scaled into it. Render resolution scales the presented base; the 1280x720 base at 16:9 is the template's and has not been measured for this game ([launcher.md](launcher.md#aspect-ratio-and-the-guest-picture)).
- **F11** toggles Borderless Fullscreen. It is SDL's default fullscreen mode, not an exclusive display mode, and it uses the current display. In the launcher, F11 switches the launcher itself instead ([launcher.md](launcher.md#window-position)).
- F11 again returns to windowed. SDL restores the previous windowed size and position.
- Auto-repeat is ignored, so holding F11 toggles once.
- ReXGlue's `Window::SetFullscreen` discards SDL's return value, so a refused switch is not reported.
- F11 switches the window only. It does not change the saved Display mode, so the next start uses the launcher's choice.
- In gameplay the mouse cursor follows the last input device in both Windowed and Borderless Fullscreen: mouse activity shows it, keyboard or controller activity hides it. It stays visible while a mouse-driven overlay is open and while the window has no focus. The launcher is unaffected.

## Title FPS

- Title: `Turtles in Time Re-Shelled Recompiled | FPS: 60.00`, refreshed once per second. Turned off by the launcher's "Frame rate in title" (`tmnt_show_fps = false`), which leaves `Turtles in Time Re-Shelled Recompiled`.
- With "Render resolution in title" on (`tmnt_show_render_resolution = true`, off by default), the title shows the render resolution between the name and the FPS: `Turtles in Time Re-Shelled Recompiled | 3360x1440 | FPS: 60.00`. That is the Render resolution chosen in Graphics, not the window size or the Internal render resolution.
- Shows the plain title until the first frame is counted.
- Shown in windowed mode. Still counted in fullscreen, where the title bar is hidden.
- Source: the title's `VdSwap` call (hooked in `src/window_host.cpp`), so the value is the number of `VdSwap` calls per second. The game has one `VdSwap` call site, in its present, so one call is one game frame; the runtime's overlays add no second swap.
- The F2 counter reads the same sample.
- The swap hook waits for the next vertical blank before the game's swap (`src/frame_limiter.cpp`), at the guest video mode's refresh rate (60 Hz), which is also the rate of ReXGlue's vblank timer. The console's present blocks the same way, so the counter reads 60 in gameplay.
