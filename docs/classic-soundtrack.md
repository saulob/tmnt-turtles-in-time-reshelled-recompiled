# Classic Soundtrack

An optional mode that plays the music of the SNES game *Teenage Mutant Ninja Turtles IV: Turtles in Time* instead of
the Re-Shelled music. It is **Off** by default.

This port does not include the SNES ROM or any music taken from it. You supply your own SNES ROM, the launcher reads
the music from it on your PC, and the result stays in your user data folder. After the import the ROM is no longer
needed.

## Turning it on

1. Open the launcher and go to **Gameplay**.
2. Choose **Import SNES soundtrack** and pick your ROM (`.sfc`, `.smc`, or a `.zip` holding one of them).
3. The launcher recognises the ROM by itself. Any supported USA, Europe or Australia ROM works, and all three give
   the same soundtrack, so you do not need to say which one you have. **SNES soundtrack** changes to **Ready**.
4. Switch **Classic soundtrack** to **On** and press Play.

| Row | What it does |
| --- | --- |
| SNES soundtrack | Not imported, Importing, Ready, or Re-import required. |
| Import SNES soundtrack | Reads a ROM and replaces any earlier import in one step. |
| Classic soundtrack | Off / On. Only available while the import is Ready, and never switched On by the import itself. |
| Remove SNES soundtrack | Deletes the imported music and turns Classic soundtrack Off. Your ROM is not touched. |

Classic soundtrack is saved the moment you change it, so it stays On the next time the game starts. While it is
active in the game, **SNES soundtrack: On** appears under the frame counter (F2). If the line is missing although
the setting is On, the game fell back to the Re-Shelled music and the log says why.

If a file cannot be used, the launcher says why: **Invalid ROM** (not a readable SNES ROM image), **Unsupported ROM**
(another SNES game), **Unsupported soundtrack** (a version whose music differs from the known releases), **Corrupt
soundtrack data** or **Import failed** (the files could not be written). A failed import keeps the previous one.

**Re-import required** means the imported files are damaged or were made by an older version of the port: import
the ROM again. Until then the game uses the Re-Shelled music.

## In the game

- **F9** switches between the SNES soundtrack and the original Re-Shelled music while you play, for A/B listening. It
  works from either starting choice as long as a SNES soundtrack is Ready, changes nothing in the launcher settings, and
  the next start uses the launcher choice again. Switching to the original is immediate. Switching back starts the SNES
  song of the music that is playing from its beginning. Without an imported soundtrack it shows **SNES soundtrack:
  Unavailable**.
- A short message appears under the frame counter (top corner without it): **SNES soundtrack: On** for 5 seconds when
  the game starts with the SNES music, then **On**, **Off** or **Unavailable** for 3 seconds after each F9. The Ping
  display moves down only while a message shows.

Only the music changes. Sound effects and voices stay as they are, and the in-game music volume still applies. If
the imported music cannot be used when the game starts, the game logs one warning and plays the Re-Shelled music.

## Where the data lives

`<user_data>/snes_soundtrack/` holds `soundset.bin` (the sound driver, samples and songs) and `manifest.toml`. Remove
it from the launcher, or delete the folder.

Current song choices: title OPEN, stages ROOF, DOWNTOWN, SEWERAGE, DINOSAUR, PIRATE, TRAIN, HIGHWAY, STAR BASE and
FORTRESS (Technodrome), boss BATTLE, stage clear CLEAR, time warp WARP2, outro STAFF. The SNES title tune is a
21-second piece and is followed by silence on the title menu.

## Testing the music

**Unlock all stages** on the launcher's Gameplay page lists every stage in Quick Play, so each stage song can be
reached without playing through the game ([launcher.md](launcher.md)). Boss, stage clear and time warp follow the
stages they belong to, and the outro follows the last boss.

## What the log says

| Line | Meaning |
| --- | --- |
| `Launcher Gameplay: ... Classic soundtrack=On` | the setting the game started with |
| `Classic soundtrack: Off` | the setting is Off |
| `Classic soundtrack: Ready, source Europe` | the import is valid |
| `Classic soundtrack: enabled, 14 music streams` | the provider is registered and the SNES music is active |
| `Classic soundtrack: the original music plays because ...` | On, but the import or the provider is not usable |
| `Classic soundtrack: StartMenu plays SNES song 6 OPEN with pack 0 at 44100 Hz` | a Re-Shelled music stream started playing an SNES song, at the stream's rate |
| `Classic soundtrack: F9 key -> SNES soundtrack: Off` | the player switched at run time |
| `Classic soundtrack: DEBUG_SAULO XMA stream not recognised, ...` | a large stream matched no known music, once per stream |

## How it works (developers)

- **Import** (`src/classic_soundtrack/importer.cpp`) reads a 1 MiB LoROM image with or without a copier header, or
  the single `.sfc`/`.smc` entry of a ZIP (miniz), checks the header and checksum, parses the sound tables, and
  accepts it only when the soundtrack fingerprint (SHA-256 over driver image, packs, songs and commands) is
  `149c010e...f7e5`. The USA, Europe and Australia SHA-256s in `known_roms.h` only name the region. The result is
  staged in `snes_soundtrack.new` and swapped in whole. Imports from importer version 1 report Re-import required.
- **Launcher** (`src/launcher.cpp`, model in `src/classic_soundtrack_page.cpp`) runs the import as a `trg::Task`,
  polls it every frame, and never identifies ROMs itself. A setting left On without a Ready import is switched Off.
- **Game start** (`src/classic_soundtrack_runtime.cpp`, `activation.cpp`, decision in `runtime_start.cpp`): with the
  setting Off nothing is loaded; with a Ready import the sound set is validated and a PCM provider is registered
  through the patched runtime (patch 0087, `rex_audio_set_xma_pcm_provider`); anything else falls back with one
  warning. `IsClassicSoundtrackActive()` is true only after the runtime took the provider, and drives the overlay.
- **Playback**: the provider recognises the 14 Re-Shelled music waves by an FNV-1a fingerprint of each XMA input
  buffer (`music_streams.h`, checked against all 1266 streams of `TMNT.bf` by `tools/xma_music_fingerprints.py`)
  and runs the game's own SPC700 driver in snes_spc (`third_party/snes_spc`, Game_Music_Emu 0.6.4, LGPL 2.1 or
  later, fast DSP). Each song boots the driver from a fresh ARAM image at `$0400` with DSP FLG `$E0` and gets its
  command (`$80` or `$A0`) on port 0. Every song uses the base sample pack 0: a stage pack only replaces samples 0-2
  with one-shot stage sound effects (its command byte plays one), which the songs would key as instruments. Output is resampled from 32 kHz to the stream's rate; the runtime only
  overwrites the samples the guest consumes, so volume, pause, loops and stops stay the game's own. The audio path
  does no file access or allocation; a song starts in about 0.1 ms.
- **Rates**: the emulator makes 32000 stereo frames a second (1,024,000 SPC clocks, one frame per 32 clocks, whatever
  the ROM region). The patched runtime tells the provider the rate of the XMA stream, the provider resamples to exactly
  that rate, and the game's own mixer carries it to the output device (48000 Hz on the reference machine). The
  provider writes exactly the stereo frames each decoded XMA block holds, in any callback size.
- **F9** only flips an atomic flag the provider reads. A recognised music stream keeps its slot while the original
  plays; patch 0087 lets the render callback pass a block through without ending the stream.

Developer commands (not needed to use the feature). `render` writes a WAV file and `analyze` prints statistics; neither
plays audio. `render ... native` is the emulator's raw 32000 Hz output, the default is the game's own path (the same
emulator, resampler and pack choice) at 44100 Hz, and `48000` is available too:

```
cmake --build build --target tmnt_classic_soundtrack_tool
build/tmnt_classic_soundtrack_tool validate game/snes
build/tmnt_classic_soundtrack_tool import|status|remove|verify-all <...>
build/tmnt_classic_soundtrack_tool render <user_data> <song_id> <seconds> <output.wav> [native|44100|48000] [pack N]
build/tmnt_classic_soundtrack_tool analyze <user_data> <song_id> <seconds>
python tools/xma_music_fingerprints.py game/extracted/Data/TMNT.bf
```

Tests use synthetic ROMs and a hand-assembled SPC700 driver only: `tmnt_classic_soundtrack_tests`,
`tmnt_classic_soundtrack_playback_tests`, `tmnt_classic_soundtrack_launcher_tests` and, against the patch stack,
`tests/xma_pcm_override_tests.cpp`, all run by `tools/test.ps1`. ROMs, SPC, WAV, OGG and FLAC files, `snes_soundtrack`
and `soundset.bin` are git-ignored and rejected by both release checks.
