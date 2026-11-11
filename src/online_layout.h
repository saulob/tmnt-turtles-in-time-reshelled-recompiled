#pragma once

#include <algorithm>
#include <cstddef>

#include "trg/layout.h"

namespace tmnt {

inline constexpr float kLobbyListMinHeight = 180.0f;
inline constexpr float kCardPaddingX = 14.0f;
inline constexpr float kCardPaddingY = 10.0f;

// The card grows with its rows between Dragon's minimum and the page's remaining height.
inline float LobbyCardHeight(float content_region_height, size_t players, float scale) {
  const float minimum = kLobbyListMinHeight * scale;
  const float available = std::max(content_region_height - 2.0f * scale, minimum);
  const float padding = kCardPaddingY * 2.0f * scale;
  const float row_text = trg::layout::kTextLabel + trg::layout::kLabelGap + trg::layout::kTextHelp;
  const float row = std::max(row_text, trg::layout::kControlHeight) + trg::layout::kRowPadding * 2.0f;
  const float content =
      players == 0 ? padding + trg::layout::kTextHelp * scale : padding + row * float(players) * scale;
  return std::min(available, std::max(content, minimum));
}

}  // namespace tmnt
