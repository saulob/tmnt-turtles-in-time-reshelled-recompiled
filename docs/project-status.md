# Project status

Version 0.2.0 is the current release (0.1.0 was the first): the game is playable on Windows, with an experimental Linux
AppImage ([linux.md](linux.md)).

## Template

The project was bootstrapped from Hard Corps: Uprising Recompiled (v0.7.1): the same CMake layout, ReXGlue SDK 0.10.0
baseline and runtime patches, TRG Launcher 1.1.0 with its patches, installer, settings, logging, crash reports,
updater, packaging and tests. Everything game-specific in that project (guest hooks, addresses, fingerprints, DLC
characters, cheats, the high frame rate stack, offline leaderboards, native aspect ratio hooks) was left out, and this
game's own hooks were written instead. The source namespace is `tmnt` and the project cvars use the `tmnt_` prefix.

## Code generation

`tools/codegen.ps1` checks both executables against their SHA-256 and runs `rexglue codegen` on
`tmnt_turtles_in_time_reshelled_manifest.toml`:

| Module | Manifest entry | Output | Result |
|---|---|---|---|
| `default.xex` | `[entrypoint]` | `generated/default` | analysis passes with 6 function seeds in `config/overrides.toml` |
| `TMNT_r.xex` | `[[modules]]`, `guest_path = "tmnt_r.xex"` | `generated/tmnt_r` | analysis passes with 1 function seed in `config/tmnt_r-overrides.toml` |

The seeds are tail-branch targets that analysis left outside any function; the generated bodies are a small leaf
function, branch thunks and a byte-copy loop. The first full run takes about 30 s and writes 382 files.

The module becomes the shared library `tmnt_turtles_in_time_reshelled_TMNT_r` (from `generated/dll_targets.cmake`),
placed next to the program. At run time ReXGlue matches the guest path the game passes to `XexLoadImage`, normalized
(device prefix removed, lower case), against `tmnt_r.xex`.

`tools/find-missing-functions.py` reads the section layout from a decrypted image of `default.xex`
(`game/analysis/default.image.bin`). `default.xex` uses LZX compression, and no tool in this repository produces that
image yet.

## Imports

Every import of both modules links to a ReXGlue 0.10.0 export. These are stubs in the SDK:

| Area | Stubbed imports |
|---|---|
| Custom I/O device | `IoCheckShareAccess`, `IoCompleteRequest`, `IoDeleteDevice`, `IoInvalidDeviceRequest`, `IoRemoveShareAccess`, `IoSetShareAccess`, `ObIsTitleObject`, `ObReferenceObject` |
| Networking | `NetDll_WSAGetOverlappedResult`, `NetDll_XNetCreateKey`, `NetDll_XNetQosLookup`, `NetDll_XNetRegisterKey`, `NetDll_XNetUnregisterKey`, `NetDll_getsockname` |
| Xbox Live UI and stats | `XamShowAchievementsUI`, `XamShowFriendsUI`, `XamShowGamerCardUIForXUID`, `XamShowMarketplaceUI`, `XamShowPlayerReviewUI`, `XamUserCreateStatsEnumerator` |
| Voice | `XamVoiceSubmitPacket` |
| Strings | `RtlUpcaseUnicodeChar` |

`IoCreateDevice` itself is implemented.

## Notes

- **Linux module loading.** ReXGlue 0.10.0 `dlopen`s the module library by its bare name, without `lib`, `.so` or a
  folder; patch 0082 makes it find `lib<name>.so` ([linux.md](linux.md)).
- **Aspect Ratio.** The game uses the original 16:9 presentation. Additional native ratios are deferred.
- **Render base.** The 1280×720 base used by the Render resolution labels and diagnostics comes from the template.
- **Achievements.** Unlocks and their persistence are ReXGlue's: the title XDBF supplies the 12 names, descriptions, icons and 200
  gamerscore, and `achievements\584109D2.toml` holds the unlock state. The game's `XamShowAchievementsUI` wrapper is bridged to the
  project's local overlay, which also handles controller input while it is open.
- **Removed menu entries.** Leaderboards (main and pause menus), XBOX LIVE PARTY (Online menu) and every reachable
  `Y GAMER CARD` hint (Lobby, Matchmaking, Summary, Game Over) are edited out of the FIRE menu movies as they are unpacked
  from the world bundles (`src/fire_movie_patch.cpp`). There is no Gamer Card overlay.
- **Leaderboards.** No menu reaches the page any more. It uses a local store at `584109D2\live\stats.toml`. Only the local player's own
  submitted score is answered, as rank 1; there are no remote players, friends or global rankings. The scripts are
  compiled into `TMNT_r.xex` and hooked there (`src/live_hooks_tmnt_r.cpp`): the local profile counts as signed in to
  Xbox Live only while the leaderboard page, its sign-in check and the leaderboard, single-player and local-multiplayer
  proceed events run. Game loading and session code keep the original local/offline answer.
- **Runtime patches.** Patch behaviour measured in Hard Corps has not been re-measured here
  ([rexglue-runtime-patch.md](rexglue-runtime-patch.md)).
