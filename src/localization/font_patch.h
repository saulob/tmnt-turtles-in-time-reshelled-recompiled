#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace tmnt::localization {

// The menu font has no a/o with tilde. Under pt-BR, ã õ Ã Õ take the place of ä ö Ä Ö, which sit next to them in the
// sorted code tables: each is drawn from the font's own letter and tilde into the free part of the player's atlas.
struct FontPatchResult {
  int descriptors = 0;
  int movie_fonts = 0;
  int atlases = 0;
};

bool IsFontBundle(size_t size);

// Leaves anything that does not match the supported game data unchanged.
FontPatchResult AddPortugueseGlyphs(uint8_t* bundle, size_t size);

struct GlyphCell {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> alpha;
};

struct ComposedGlyph {
  GlyphCell cell;
  int rise = 0;  // rows the tilde adds above the base glyph
};

// Replaces the accent above `base` with the one above `accent_source`, centred where the old accent was. Empty when a
// glyph is too short to hold an accent.
std::optional<ComposedGlyph> ReplaceAccent(const GlyphCell& base, const GlyphCell& accent_source);

void DecodeDxt5Alpha(const uint8_t* block, uint8_t pixels[16]);
void EncodeDxt5Alpha(const uint8_t pixels[16], uint8_t* block);

}  // namespace tmnt::localization
