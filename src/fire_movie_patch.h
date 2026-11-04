#pragma once

#include <cstddef>
#include <cstdint>

namespace tmnt {

// One byte of a movie: the value it must hold and the value it gets.
struct MovieBytePatch {
  size_t offset;
  uint8_t before;
  uint8_t after;
};

// Applies every patch or none: false when the buffer is too small or a byte is not what the patch expects.
bool ApplyMovieBytePatches(uint8_t* movie, size_t size, const MovieBytePatch* patches, size_t count);

// The title's Online menu movie (FIRELAB_online.feu) lists Quick, Create and Custom Match and the Xbox LIVE party row.
// Its own PS3 branch hides the party row and ends the tab list at the third entry; the patch takes that branch and
// stops the movie asking for the party row's text. Only the unmodified movie is patched.
inline constexpr size_t kOnlineMenuMovieBytes = 115401;
inline constexpr MovieBytePatch kOnlineMenuPatches[] = {
    {6054, 30, 0},  // the console-type If stays, so it still pops its condition; its else jump lands on the PS3 branch
    {6150, 4, 3},   // the tab count the movie starts with
    {2112, 4, 3},   // the texts requested from the first menu row on
};
bool PatchOnlineMenuMovie(uint8_t* movie, size_t size);

}  // namespace tmnt
