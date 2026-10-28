# Project status

**Early development.** The project structure, launcher, installer and codegen are in place. The game has not been
built or run yet, so nothing below claims that it boots or is playable.

## Template

The project was bootstrapped from Hard Corps: Uprising Recompiled (v0.7.1): the same CMake layout, ReXGlue SDK 0.10.0
baseline and runtime patches, TRG Launcher 1.1.0 with its patches, installer, settings, logging, crash reports,
updater, packaging and tests. Everything game-specific in that project (guest hooks, addresses, fingerprints, DLC
characters, cheats, skip intro, the high frame rate stack, offline leaderboards, native aspect ratio hooks) was left
out. The source namespace is `tmnt` and the project cvars use the `tmnt_` prefix.

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
(device prefix removed, lower case), against `tmnt_r.xex`. The exact path the game uses has not been observed yet.

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

`IoCreateDevice` itself is implemented. The game registering its own device driver is the import pattern most likely to
need attention at first boot.

## Known blockers and unknowns

- **Linux module loading.** ReXGlue 0.10.0 `dlopen`s the module library by its bare name, without `lib`, `.so` or a
  folder ([linux.md](linux.md)). The Windows build is not affected.
- **First boot not attempted.** The module load path, the custom I/O device, `Data/TMNT.bf` access and the shader
  handling are all unverified.
- **Aspect Ratio.** Only the generic video mode and letterbox are applied; non-16:9 output is unverified.
- **Render base.** The 1280×720 base used by the Render resolution labels and diagnostics comes from the template.
- **FPS counter.** Counts every `VdSwap`; whether the game repeats presents is unknown.
- **Achievements.** The F7 overlay and the launcher page read the title XDBF through ReXGlue; nothing calls the
  template's achievement-0 alias, and `ArcadeInfo.xml` numbers the achievements from 1.
- **Runtime patches.** Patch behaviour measured in Hard Corps has not been re-measured here
  ([rexglue-runtime-patch.md](rexglue-runtime-patch.md)).

## Graphics strategy

The baseline is the template's: ReXGlue's Xenos GPU plugin (D3D12 on Windows, Vulkan on Linux) with the project's
runtime patches. It is the compatibility and reference renderer. A native renderer path (ReXGlue with Plume and
XenosRecomp) is not part of this project yet; it can be evaluated once the Xenos path renders the game, against the
same captures.
