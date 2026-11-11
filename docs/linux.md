# Linux and Steam Deck

> **Experimental.** Windows is the primary platform. The AppImage has not yet been validated on real Linux or Steam Deck
> hardware, so rendering, audio, controllers and online play on Linux are unconfirmed.

The Linux build uses the same code as the Windows build: ReXGlue SDK 0.10.0's Linux runtime and the Xenos GPU
plugin's Vulkan backend, both built with the project's patches, and the same TRG launcher. It ships as one
AppImage, which is also the Steam Deck build. It is native Linux, not Proton or Wine.

Every launcher setting works on Linux. **Graphics card** lists the Vulkan hardware devices (the dedicated GPU first). Until
one is chosen the driver's own selection stands. A crash appends a `CRASH REPORT` block to the session log, as on Windows,
with the signal number beside the matching Windows exception code. `libssl` is loaded at run time for the online service,
so a system without OpenSSL can play everything but online.

**The module library.** The game's code is split between `default.xex` and the module `TMNT_r.xex`, which is recompiled
into its own shared library (`libtmnt_turtles_in_time_reshelled_TMNT_r.so`). ReXGlue 0.10.0 opens it with
`dlopen("tmnt_turtles_in_time_reshelled_TMNT_r")`, without the `lib` prefix or the `.so` suffix, which `LoadLibrary`
completes on Windows but `dlopen` does not. The Linux-only patch `0082-posix-recompiled-module-library` makes the runtime
look for `lib<name>.so` beside the executable first.

The cyan turtle flicker fix (`0080`) has its Vulkan counterpart in `0091`, not yet confirmed on a Steam Deck. One
Direct3D 12 fix that a player can see still has none: vertex buffers rewritten at the same address (`0079`), which can
show as stale geometry; the temporary `0092` telemetry logs when that happens. The Vulkan equivalents of `0040` and
`0055` are deferred too; those two are performance work. These three are parity `vulkan-candidate` in
`patches/rexglue-sdk/patches.txt`.

The AppImage FUSE fallback (the image unpacks itself and runs from there when it cannot be mounted) is part of the
inherited runtime ([Steam Beta and the AppImage](#steam-beta-and-the-appimage)).

## Requirements

- 64-bit x86 Linux (x86_64) with glibc 2.31 or newer, such as Ubuntu 20.04 or later (`ldd --version` shows it)
- A Vulkan driver (Mesa RADV on a Steam Deck and most AMD and Intel PCs)
- An X11 or Wayland desktop (the runtime loads `libX11`, `libX11-xcb`, `libxcb` and `libwayland-client`)
- The desktop file chooser portal (`xdg-desktop-portal`), `zenity` or `kdialog` for the package picker, and `xdg-open` for the About page's folder buttons
- Your own Turtles in Time Re-Shelled Xbox Live Arcade package (see the README for the exact version)

The AppImage carries the C++ runtime of its build machine (GCC 13 or newer). It is used only when the system's own copy
is older than GCC 13.

## Installing

1. Download **TurtlesInTimeReshelledRecompiled-v…-x86_64.AppImage** and put it in a folder you can write to.
2. Make it executable: right-click it > **Properties** > **Permissions** > **Is executable**, or
   `chmod +x TurtlesInTimeReshelledRecompiled-v*-x86_64.AppImage`.
3. Run it. The launcher opens.
4. On the **Play** page click **Install from package...** and pick your package. The game files are copied into a
   `game` folder next to the AppImage.
5. Press **PLAY**.

### Steam Deck

This is the intended flow. It has not been tried on a Steam Deck yet.

1. In **Desktop Mode**, do steps 1 to 5 above. Copy your package to the Deck first (USB stick, SD card or network).
2. In Steam: **Games** > **Add a Non-Steam Game to My Library**, and pick the AppImage.
3. Return to **Game Mode** and start it from your library.

Use the touchscreen or the trackpads in the launcher. A controller's **Start** button presses Play, and the D-pad,
left stick, A and B move through the launcher.

### Steam Beta and the AppImage

An AppImage mounts itself with FUSE before any of its own code runs. On Steam's Beta client, a non-Steam AppImage can be
started where that mount is not possible, in Game Mode and in Desktop Mode alike. It then stops at once, Steam's log shows
"Cannot mount AppImage, please check your FUSE setup.", and the game writes no log because it never starts, while the same
file started from a file manager works. Other AppImages are reported to fail the same way
([ValveSoftware/steam-for-linux#13736](https://github.com/ValveSoftware/steam-for-linux/issues/13736)).

The AppImage deals with this itself, with nothing to set in Steam. Its runtime still tries the FUSE mount
first. If the mount cannot be made, it unpacks the AppImage into a private folder under `/tmp`, runs the game
from there with the same arguments, returns the game's exit status and removes the folder afterwards. A restart of the game
from the launcher starts the original AppImage again and goes through the same steps. Where FUSE works, the AppImage is
mounted as before and nothing is unpacked. Only a failed mount triggers this: a damaged AppImage is still reported as
damaged. `game`, `logs` and `user_data` stay next to the AppImage in both cases.

`tools/test-appimage-release.sh` verifies this on a fixture AppImage with FUSE out of reach; it has not been tried on
SteamOS. A line such as `Error: $FUSERMOUNT_PROG not set` can still appear in the terminal or Steam's log; it is expected
and harmless.

If an AppImage still does not start from Steam, open the game's Steam **Properties**, set **Launch Options** to

```
APPIMAGE_EXTRACT_AND_RUN=1 %command%
```

and tell us whether that changes anything. It makes the runtime unpack the AppImage without trying FUSE at all
(`%command% --appimage-extract-and-run` does the same).

## Where things are

| What | Where |
| --- | --- |
| Game files | `game` next to the AppImage |
| Logs | `logs` next to the AppImage (`~/.local/share/tmnt_turtles_in_time_reshelled/logs` if that folder is not writable) |
| Settings, saves, shader cache | `user_data` next to the AppImage ([user-data.md](user-data.md)) |

The AppImage mount is read-only and gone once the program exits, so nothing is written there: every folder above follows the
`APPIMAGE` path, and saves stay in place when the AppImage is moved to a new version in the same folder or updated by the
launcher. ReXGlue's own Linux location, `$XDG_DATA_HOME/tmnt_turtles_in_time_reshelled`, moves into `user_data` at the first
start if it holds data. A build run outside an AppImage keeps `game`, `logs` and `user_data` next to the program instead. `--game_data_root` and `--user_data_root` override the folders, as on Windows.

- **Window icon.** The program sets its window icon from the AppImage's own icon file, so it does not depend on the
  AppImage being integrated into the desktop. A build run outside an AppImage keeps the desktop's default icon.
- **Application ID.** The program tells the desktop it is `io.github.saulob.tmnt-turtles-in-time-reshelled` (the Wayland
  app_id and the X11 window class). The `.desktop` file in `packaging/linux` has the same name and sets it as
  `StartupWMClass`, and `tools/package-linux.sh` stops if they differ.

## Known limits

- Not tested on real hardware yet: rendering, audio, controllers and performance are unconfirmed.
- The launcher updates the AppImage only under its release file name; a renamed file is left alone.
- Holding **Shift** to bring the launcher back works only if the window gets keyboard focus while Shift is still
  held, because SDL cannot read the keyboard before the window has focus. Keep Shift held until the launcher appears.
- The VSync setting maps to Vulkan's present mode: **Off** allows immediate presentation (tearing), **On** uses
  mailbox or FIFO.
- On a Wayland desktop with display scaling above 100%, window sizes can come out larger than chosen. ReXGlue
  sizes its window this way on Linux.

## Building the AppImage

Run on Linux or WSL, from a clone with the TRG launcher submodule, after `tools/codegen.ps1` has generated
`generated/default` and `generated/tmnt_r` and with your own `game/extracted/default.xex` and `TMNT_r.xex`. The build runs the
patched `rexglue` code generator again, which reads both executables; neither is packaged:

```sh
tools/package-linux.sh            # incremental Release build, then package
tools/package-linux.sh --clean    # clean Release build first
```

It needs `clang-20`, `clang++-20`, `lld-20` (the game links with ThinLTO), CMake 3.25+, Ninja, rsync, wget or curl,
unzip or python3, binutils, the GCC 13 or newer C++ runtime and headers, and the X11/Wayland client libraries
(`libx11-6`, `libx11-xcb1`, `libxcb1`, `libwayland-client0`), which the SDK's `rexglue` code generator loads.

The AppImage needs the glibc of the machine that built it, so releases are built on Ubuntu 20.04 (glibc 2.31). The
script stops on any other host (`TMNT_ALLOW_UNSUPPORTED_HOST=1` skips only that check) and runs
`tools/check-glibc-baseline.sh` on the AppDir and again on the finished, extracted AppImage. The checker recognises ELF files
by their magic bytes, fails on a `GLIBC_` symbol above 2.31 or a bundled glibc runtime library, prints the highest `GLIBC`,
`GLIBCXX` and `CXXABI` version of every file, and fails when the bundled `libstdc++` provides less than another file needs.
The same scan (forbidden names and types, Xbox packages, `DEBUG_SAULO`, local folders as text and as UTF-16, RenderDoc, the
Vulkan loader and layers, RPATH/RUNPATH, NEEDED libraries, debug sections of all four binaries, the exact file list) runs on both.
`tools/linux-policy.sh` holds that policy. The debug information of the program and the module goes to
`<out dir>/TurtlesInTimeReshelledRecompiled-v<version>-x86_64-symbols/`, which is not packaged.
`tools/package-linux.sh --verify-only FILE` runs the scan, the runtime checks and the glibc check on an existing AppImage.

`tools/test.ps1` is the headless Windows/Linux parity gate (patch manifest, patch apply for both series, the audio client teardown of patch 0086, packaging inputs,
the shared feature matrix in `tools/parity-manifest.txt`, hygiene); it launches nothing. `tools/test.sh` runs the same
groups from Linux and adds the shell and compiler checks. The real-hardware checks are in
[linux-test-checklist.md](linux-test-checklist.md). `tools/test-appimage-release.sh` staged-fixture tests run in a focal WSL
distribution; set `TMNT_APPIMAGE_TOOLS` to a folder holding appimagetool and a built runtime to add a fixture AppImage.
`tools/setup-linux-build-env.sh` prepares a clean Ubuntu 20.04 machine for it, as root: clang 20 from
[apt.llvm.org](https://apt.llvm.org), the GCC 13 runtime from the `ubuntu-toolchain-r/test` PPA, CMake 3.31.8 and the
SDK's build dependencies. libdecor 0.2.2 and PipeWire 0.3.48 are not packaged for 20.04, so the script builds them
from their pinned commits into `/usr/local`; SDL loads both at run time and needs only their headers to build the
Wayland decoration and PipeWire audio backends, so the AppImage does not carry them. To make the environment in WSL:

```sh
wsl --import TmntFocalBuild <folder> ubuntu-base-20.04.5-base-amd64.tar.gz
wsl -d TmntFocalBuild -u root -- bash tools/setup-linux-build-env.sh
```

The base image is `ubuntu-base-20.04.5-base-amd64.tar.gz` from cdimage.ubuntu.com (SHA-256
`60e216b60947653dc8989be3821380268315b99b2959c29882781541bfe5a426`); a Docker `ubuntu:20.04` container works the same
way. The build runs `rexglue`, so `tools/package-linux.sh` builds it from the patched source, the same generator Windows uses;
the SDK's stock tool lacks those patches and needs glibc 2.35.

The script copies the sources to `~/tmnt-linux-build` (set `TMNT_WORK` to change it), downloads the pinned ReXGlue
SDK and appimagetool with their SHA-256 checks, builds the project's patched ReXGlue runtime and
Vulkan GPU plugin from source the first time and whenever the patches change
([docs/rexglue-runtime-patch.md](rexglue-runtime-patch.md) lists the extra packages and the network access this
needs), builds the AppImage runtime (below), builds the `linux-amd64-release` preset, checks
that the AppDir holds no game files, packages, saves, logs or local paths, and writes
`dist/TurtlesInTimeReshelledRecompiled-v<version>-x86_64.AppImage` with its `.sha256` (`--out-dir DIR` writes them to DIR instead of `dist`). It publishes nothing.
It prints the commit it was run on and says when the working tree has uncommitted changes: release builds are made from the
committed tree.

### The AppImage runtime

The runtime is the small program at the front of every AppImage that mounts it and starts AppRun. The project builds its
own from the upstream [AppImage/type2-runtime](https://github.com/AppImage/type2-runtime) source, because the released
runtime stops when FUSE cannot mount the AppImage (see [Steam Beta and the AppImage](#steam-beta-and-the-appimage)):

- `patches/appimage-runtime/baseline.env` pins the upstream commit (`dd6cebe`, tag `20251108`, the revision of the
  runtime the project used before), the Alpine 3.21.0 root file system the build runs in, and the SHA-256 of the
  reviewed result.
- `patches/appimage-runtime/0001-extract-and-run-when-fuse-fails.patch` is the whole change: when the FUSE mount fails
  the runtime uses its own extract-and-run code instead (the one `APPIMAGE_EXTRACT_AND_RUN` selects); each run gets a
  private extraction folder, termination signals reach the game, and the folder is really removed afterwards
  (upstream's cleanup does nothing on the musl build, whose `nftw` ignores a descriptor limit of 0).
- `tools/build-appimage-runtime.sh` fetches that commit, checks it, applies the patch and runs upstream's own chroot
  build (needs root or sudo, git, tar and network access). The result is checked (type 2 magic, static, the version
  string) and compared with the pinned SHA-256; with the same Alpine packages two builds give identical files. If the
  checksum differs, packaging stops: review the new build and update `RUNTIME_SHA256`. `--check FILE` verifies a runtime or
  a finished AppImage, and `--check-image FILE` that the runtime at the front of an AppImage is byte for byte the reviewed one.

A build prints, and saves next to the runtime as `<runtime>.manifest`, what produced it: the upstream commit, the patch's SHA-256,
the Alpine version, the compiler, linker and library package versions apk resolved, and the runtime's SHA-256. The runtime's
static components and their licenses are listed in `THIRD-PARTY-LICENSES-LINUX.txt` inside the AppImage.

The runtime is rebuilt only when the pins or the patch change. Set `VERBOSE=1` when starting an AppImage to see which path it
took: a message appears when it falls back to extracting.

For a plain build without packaging, with the SDK extracted into `tools/rexglue/linux-amd64`, `tools/build.sh`
(`release` or `relwithdebinfo`) builds the patched ReXGlue libraries and code generator when they are missing or out of date
and then the `linux-amd64-<config>` preset into `out/build/<preset>`. There is no Debug build on Linux: the stock Debug
runtime has no patch 0082, so it cannot load the module library. Building a preset directly works too, but stops if
the SDK does not hold the patched libraries.
