# snes_spc (from Game_Music_Emu)

The SPC700 CPU and S-DSP emulator by Shay Green (blargg), as shipped in Game_Music_Emu 0.6.4:

- Source: https://github.com/libsdl-org/game-music-emu, commit `8ebb4f65096fdf980b39be2283699d523ddc09af`
- Files: `Snes_Spc`, `Spc_Cpu`, `Spc_Dsp`, `blargg_common.h`, `blargg_config.h`, `blargg_endian.h`, `blargg_source.h`
- License: GNU LGPL 2.1 or later (`license.txt`). The game links it statically; the whole program's source is
  published under GPL-2.0-only, which LGPL 2.1 section 3 permits.
- Unmodified. The build defines `BLARGG_LITTLE_ENDIAN=1`, as the library's own CMake does on little-endian hosts.

This is Game_Music_Emu's fast DSP (`SPC_LESS_ACCURATE`), not the cycle-accurate `SPC_DSP` of the standalone
snes_spc 0.9.0 release, which is no longer downloadable from its author's site.
