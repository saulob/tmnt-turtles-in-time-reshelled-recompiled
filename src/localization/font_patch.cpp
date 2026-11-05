#include "localization/font_patch.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <string_view>

namespace tmnt::localization {
namespace {

constexpr size_t kBundleMenus = 19784989;     // f88d0000.bin
constexpr size_t kBundleLoading = 3984900;    // f88d0001.bin, the only one holding the descriptor
constexpr size_t kBundleGame = 15331229;      // f88d0002.bin

constexpr size_t kDescriptorOffset = 72;
constexpr size_t kDescriptorBytes = 4854;
constexpr size_t kDescriptorHeaderBytes = 48;
constexpr size_t kDescriptorRowBytes = 27;
constexpr size_t kDescriptorGlyphs = 178;
constexpr uint64_t kDescriptorHash = 0x9C76A89020AF89D7ull;

constexpr int kAtlasSide = 512;
constexpr size_t kAtlasBytes = 262144;
constexpr uint64_t kAtlasHash = 0x6A28C7B335B2EE0Aull;
constexpr size_t kAtlasHeaderBytes = 24;
constexpr uint8_t kAtlasHeader[kAtlasHeaderBytes] = {0x96, 0x04, 0xA0, 0x05, 0x10, 0x00, 0x04, 0x00, 0x0D, 0x00, 0x04, 0x00,
                                                     0x01, 0x00, 0x00, 0x00, 0x0B, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02};
constexpr size_t kBlockBytes = 16;
constexpr size_t kAlphaBytes = 8;
// Every glyph is white; only its alpha differs.
constexpr uint8_t kWhiteColorBlock[kBlockBytes - kAlphaBytes] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0};

constexpr uint8_t kInkAlpha = 24;

constexpr char kFontSignature[] = "\x84\x01\x0ATMNTredux";
constexpr size_t kFontSignatureBytes = sizeof kFontSignature;  // includes the name's terminator
constexpr size_t kFontTagToSignature = 2;
constexpr size_t kFontGlyphCount = 15;
constexpr size_t kFontOffsets = 17;

struct Rect {
  int x, y, width, height;
};

struct GlyphSwap {
  uint16_t dropped;
  uint16_t added;
  uint16_t base;  // the glyph whose letter is kept and whose metrics and advance are taken
  Rect base_rect;
  Rect tilde_rect;
  int base_left;
  int base_top;
  int cell_x;
};

constexpr int kCellY = 4;
constexpr Rect kUpperTilde = {199, 468, 28, 39};  // Ñ
constexpr Rect kLowerTilde = {161, 424, 28, 38};  // ñ

constexpr GlyphSwap kSwaps[] = {
    {0xC4, 0xC3, 0xC2, {161, 231, 27, 40}, kUpperTilde, 2, 11, 64},   // Ä -> Ã from Â
    {0xD6, 0xD5, 0xD4, {453, 425, 33, 39}, kUpperTilde, 2, 13, 112},  // Ö -> Õ from Ô
    {0xE4, 0xE3, 0xE2, {161, 305, 30, 36}, kLowerTilde, 2, 14, 160},  // ä -> ã from â
    {0xF6, 0xF5, 0xF4, {199, 428, 29, 38}, kLowerTilde, 1, 14, 208},  // ö -> õ from ô
};

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

uint16_t Load16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

void Store16(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
}

void StoreFloat(uint8_t* p, float value) { std::memcpy(p, &value, sizeof value); }

// Rows are stored bottom-up, so a glyph's row y is atlas row kAtlasSide - 1 - y.
size_t BlockOffset(int x, int row) { return size_t(row / 4 * (kAtlasSide / 4) + x / 4) * kBlockBytes; }

uint8_t ReadAlpha(const uint8_t* atlas, int x, int y) {
  const int row = kAtlasSide - 1 - y;
  uint8_t pixels[16];
  DecodeDxt5Alpha(atlas + BlockOffset(x, row), pixels);
  return pixels[(row % 4) * 4 + x % 4];
}

GlyphCell ReadCell(const uint8_t* atlas, const Rect& rect) {
  GlyphCell cell{rect.width, rect.height, std::vector<uint8_t>(size_t(rect.width) * rect.height)};
  for (int y = 0; y < rect.height; ++y) {
    for (int x = 0; x < rect.width; ++x) {
      cell.alpha[size_t(y) * rect.width + x] = ReadAlpha(atlas, rect.x + x, rect.y + y);
    }
  }
  return cell;
}

int RowInk(const GlyphCell& cell, int y) {
  const uint8_t* row = cell.alpha.data() + size_t(y) * cell.width;
  return int(std::count_if(row, row + cell.width, [](uint8_t a) { return a > kInkAlpha; }));
}

struct AccentRows {
  int top;
  int bottom;  // the row separating the accent from the letter
};

// Some accents nearly touch their letter, so they are split at the emptiest row of the glyph's upper third.
std::optional<AccentRows> FindAccent(const GlyphCell& cell) {
  int top = 0;
  while (top < cell.height && RowInk(cell, top) == 0) ++top;
  constexpr int kAccentMinRows = 3;
  const int first = top + kAccentMinRows;
  const int last = cell.height / 3;
  if (first > last) return std::nullopt;
  int bottom = first;
  for (int y = first; y <= last; ++y) {
    if (RowInk(cell, y) < RowInk(cell, bottom)) bottom = y;
  }
  return AccentRows{top, bottom};
}

struct InkSpan {
  int left;
  int right;
};

InkSpan AccentInk(const GlyphCell& cell, const AccentRows& rows) {
  InkSpan span{cell.width, -1};
  for (int y = rows.top; y < rows.bottom; ++y) {
    for (int x = 0; x < cell.width; ++x) {
      if (cell.alpha[size_t(y) * cell.width + x] > kInkAlpha) {
        span.left = std::min(span.left, x);
        span.right = std::max(span.right, x);
      }
    }
  }
  return span;
}

bool CellIsFree(const uint8_t* atlas, const Rect& rect) {
  for (int y = rect.y; y < rect.y + rect.height; ++y) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
      if (ReadAlpha(atlas, x, y) != 0) return false;
    }
  }
  return true;
}

void WriteCell(uint8_t* atlas, const GlyphCell& cell, int cell_x) {
  const int first_row = kAtlasSide - kCellY - cell.height;
  const int last_row = kAtlasSide - 1 - kCellY;
  for (int block_row = first_row / 4; block_row <= last_row / 4; ++block_row) {
    for (int block_x = cell_x / 4; block_x <= (cell_x + cell.width - 1) / 4; ++block_x) {
      uint8_t pixels[16] = {};
      for (int i = 0; i < 16; ++i) {
        const int x = block_x * 4 + i % 4 - cell_x;
        const int y = kAtlasSide - 1 - (block_row * 4 + i / 4) - kCellY;
        if (x >= 0 && x < cell.width && y >= 0 && y < cell.height) pixels[i] = cell.alpha[size_t(y) * cell.width + x];
      }
      uint8_t* block = atlas + BlockOffset(block_x * 4, block_row * 4);
      EncodeDxt5Alpha(pixels, block);
      std::memcpy(block + kAlphaBytes, kWhiteColorBlock, sizeof kWhiteColorBlock);
    }
  }
}

struct Composed {
  std::array<ComposedGlyph, std::size(kSwaps)> glyphs;
};

std::optional<Composed> ComposeAll(const uint8_t* atlas) {
  Composed composed;
  for (size_t i = 0; i < std::size(kSwaps); ++i) {
    const GlyphSwap& swap = kSwaps[i];
    auto glyph = ReplaceAccent(ReadCell(atlas, swap.base_rect), ReadCell(atlas, swap.tilde_rect));
    if (!glyph || !CellIsFree(atlas, {swap.cell_x, kCellY, glyph->cell.width, glyph->cell.height})) return std::nullopt;
    composed.glyphs[i] = std::move(*glyph);
  }
  return composed;
}

uint8_t* FindAtlas(std::span<uint8_t> bundle) {
  const std::string_view text(reinterpret_cast<const char*>(bundle.data()), bundle.size());
  const size_t found = text.find(std::string_view(reinterpret_cast<const char*>(kAtlasHeader), kAtlasHeaderBytes));
  if (found == std::string_view::npos || bundle.size() - found < kAtlasHeaderBytes + kAtlasBytes) return nullptr;
  uint8_t* atlas = bundle.data() + found + kAtlasHeaderBytes;
  return Fnv1a64(atlas, kAtlasBytes) == kAtlasHash ? atlas : nullptr;
}

bool PatchDescriptor(uint8_t* descriptor, const Composed& composed) {
  uint8_t* rows = descriptor + kDescriptorHeaderBytes;
  std::array<uint8_t*, std::size(kSwaps)> found{};
  for (size_t row = 0; row < kDescriptorGlyphs; ++row) {
    uint8_t* glyph = rows + row * kDescriptorRowBytes;
    for (size_t i = 0; i < std::size(kSwaps); ++i) {
      if (Load16(glyph) == kSwaps[i].added) return false;
      if (Load16(glyph) == kSwaps[i].dropped) found[i] = glyph;
    }
  }
  if (std::find(found.begin(), found.end(), nullptr) != found.end()) return false;
  for (size_t i = 0; i < std::size(kSwaps); ++i) {
    const GlyphSwap& swap = kSwaps[i];
    const GlyphCell& cell = composed.glyphs[i].cell;
    uint8_t* glyph = found[i];
    Store16(glyph, swap.added);
    StoreFloat(glyph + 3, float(swap.cell_x) / kAtlasSide);
    StoreFloat(glyph + 7, float(kCellY) / kAtlasSide);
    StoreFloat(glyph + 11, float(swap.cell_x + cell.width) / kAtlasSide);
    StoreFloat(glyph + 15, float(kCellY + cell.height) / kAtlasSide);
    Store16(glyph + 19, uint32_t(swap.base_left));
    Store16(glyph + 21, uint32_t(swap.base_top - composed.glyphs[i].rise));
    Store16(glyph + 23, uint32_t(cell.width));
    Store16(glyph + 25, uint32_t(cell.height));
  }
  return true;
}

// DefineFont3: the code table is sorted and each dropped code's neighbour takes its slot, so no table is reordered.
bool PatchMovieFont(uint8_t* tag, size_t available) {
  if (available < kFontOffsets) return false;
  const size_t count = Load16(tag + kFontGlyphCount);
  if (available < kFontOffsets + (count + 1) * 2) return false;
  const size_t codes = kFontOffsets + size_t(Load16(tag + kFontOffsets + count * 2));
  constexpr size_t kAscentDescentLeading = 6;
  const size_t advances = codes + count * 2 + kAscentDescentLeading;
  if (available < advances + count * 2) return false;

  const auto index_of = [&](uint16_t code) -> std::optional<size_t> {
    for (size_t i = 0; i < count; ++i) {
      if (Load16(tag + codes + i * 2) == code) return i;
    }
    return std::nullopt;
  };
  for (const GlyphSwap& swap : kSwaps) {
    if (!index_of(swap.dropped) || !index_of(swap.base) || index_of(swap.added)) return false;
  }
  for (const GlyphSwap& swap : kSwaps) {
    const size_t slot = *index_of(swap.dropped);
    const uint16_t advance = Load16(tag + advances + *index_of(swap.base) * 2);
    Store16(tag + codes + slot * 2, swap.added);
    Store16(tag + advances + slot * 2, advance);
  }
  return true;
}

int PatchMovieFonts(std::span<uint8_t> bundle) {
  int patched = 0;
  const std::string_view signature(kFontSignature, kFontSignatureBytes);
  const std::string_view text(reinterpret_cast<const char*>(bundle.data()), bundle.size());
  for (size_t at = text.find(signature); at != std::string_view::npos; at = text.find(signature, at + 1)) {
    if (at < kFontTagToSignature) continue;
    const size_t tag = at - kFontTagToSignature;
    if (PatchMovieFont(bundle.data() + tag, bundle.size() - tag)) ++patched;
  }
  return patched;
}

}  // namespace

void DecodeDxt5Alpha(const uint8_t* block, uint8_t pixels[16]) {
  const int a0 = block[0];
  const int a1 = block[1];
  uint8_t levels[8] = {uint8_t(a0), uint8_t(a1)};
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) levels[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i) levels[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
    levels[6] = 0;
    levels[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 5; i >= 0; --i) bits = bits << 8 | block[2 + i];
  for (int i = 0; i < 16; ++i) pixels[i] = levels[(bits >> (3 * i)) & 7];
}

void EncodeDxt5Alpha(const uint8_t pixels[16], uint8_t* block) {
  const uint8_t high = *std::max_element(pixels, pixels + 16);
  const uint8_t low = *std::min_element(pixels, pixels + 16);
  block[0] = high;
  block[1] = low;
  uint8_t levels[8] = {high, low};
  for (int i = 1; i < 7; ++i) levels[i + 1] = uint8_t(((7 - i) * high + i * low + 3) / 7);
  uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    uint64_t best = 0;
    int best_error = 256;
    for (int level = 0; level < (high == low ? 1 : 8); ++level) {
      const int error = std::abs(int(pixels[i]) - int(levels[level]));
      if (error < best_error) {
        best_error = error;
        best = uint64_t(level);
      }
    }
    bits |= best << (3 * i);
  }
  for (int i = 0; i < 6; ++i) block[2 + i] = uint8_t(bits >> (8 * i));
}

std::optional<ComposedGlyph> ReplaceAccent(const GlyphCell& base, const GlyphCell& accent_source) {
  const auto base_rows = FindAccent(base);
  const auto source_rows = FindAccent(accent_source);
  if (!base_rows || !source_rows) return std::nullopt;

  const int accent_height = source_rows->bottom - source_rows->top;
  const int rise = std::max(0, accent_height - base_rows->bottom);
  ComposedGlyph result;
  result.rise = rise;
  result.cell = {base.width, base.height + rise, std::vector<uint8_t>(size_t(base.width) * (base.height + rise))};

  for (int y = base_rows->bottom + 1; y < base.height; ++y) {
    std::memcpy(result.cell.alpha.data() + size_t(y + rise) * base.width, base.alpha.data() + size_t(y) * base.width,
                size_t(base.width));
  }

  const InkSpan base_ink = AccentInk(base, *base_rows);
  const InkSpan source_ink = AccentInk(accent_source, *source_rows);
  const int shift = (base_ink.left + base_ink.right - source_ink.left - source_ink.right) / 2;
  const int y_shift = base_rows->bottom + rise - source_rows->bottom;
  for (int y = source_rows->top; y < source_rows->bottom; ++y) {
    for (int x = 0; x < accent_source.width; ++x) {
      const int to_x = x + shift;
      if (to_x < 0 || to_x >= base.width) continue;
      result.cell.alpha[size_t(y + y_shift) * base.width + to_x] = accent_source.alpha[size_t(y) * accent_source.width + x];
    }
  }
  return result;
}

bool IsFontBundle(size_t size) { return size == kBundleMenus || size == kBundleLoading || size == kBundleGame; }

FontPatchResult AddPortugueseGlyphs(uint8_t* bundle, size_t size) {
  FontPatchResult result;
  if (!bundle || !IsFontBundle(size)) return result;
  const std::span<uint8_t> bytes(bundle, size);
  uint8_t* atlas = FindAtlas(bytes);
  if (!atlas) return result;
  const std::optional<Composed> composed = ComposeAll(atlas);
  if (!composed) return result;

  if (size == kBundleLoading) {
    uint8_t* descriptor = bundle + kDescriptorOffset;
    if (Fnv1a64(descriptor, kDescriptorBytes) != kDescriptorHash || !PatchDescriptor(descriptor, *composed)) return result;
    result.descriptors = 1;
  }
  result.movie_fonts = PatchMovieFonts(bytes);
  for (size_t i = 0; i < std::size(kSwaps); ++i) WriteCell(atlas, composed->glyphs[i].cell, kSwaps[i].cell_x);
  result.atlases = 1;
  return result;
}

}  // namespace tmnt::localization
