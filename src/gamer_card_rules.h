#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "overlay_rules.h"

namespace tmnt {

constexpr uint32_t kGamerCardUsers = 4;
constexpr char kGamerCardGame[] = "Turtles in Time Re-Shelled";

// Leaderboard_Defs.h: the two score leaderboards the game lists as STORY MODE and SURVIVAL MODE.
constexpr uint32_t kStoryScoreView = 1;
constexpr uint32_t kSurvivalScoreView = 2;

// An id of zero names no player: the card asked for is the user's own.
constexpr uint64_t kUnspecifiedXuid = 0;

struct GamerCardRequest {
  uint32_t user_index = 0;
  uint64_t xuid = kUnspecifiedXuid;
};

// Everything the card can show, read once when it opens. Empty or absent means the profile does not hold it.
struct GamerCardSources {
  uint64_t local_xuid = 0;
  std::array<std::string, kGamerCardUsers> names;
  std::optional<AchievementSummary> achievements;
  std::optional<int64_t> story_score;
  std::optional<int64_t> survival_score;
  std::string motto;
  int32_t zone = 0;
  int32_t region = 0;
};

struct GamerCardRow {
  std::string label;
  std::string value;
  bool operator==(const GamerCardRow&) const = default;
};

struct GamerCardView {
  bool available = false;
  std::string gamertag;
  std::string status;
  std::vector<GamerCardRow> profile_rows;
  std::string game;
  std::vector<GamerCardRow> game_rows;
};

// The Summary and Leaderboards screens gate the card on the profile's Live privileges, which a local player lacks.
constexpr bool IsLocalGamerCardUser(uint32_t user_index) { return user_index < kGamerCardUsers; }

// A card is shown for a signed-in local user, asked for by its own id or by none. Any other id belongs to a player
// this machine knows nothing about.
inline bool IsLocalGamerCard(const GamerCardRequest& request, const GamerCardSources& sources) {
  if (!IsLocalGamerCardUser(request.user_index) || sources.names[request.user_index].empty()) return false;
  return request.xuid == kUnspecifiedXuid || request.xuid == sources.local_xuid;
}

// XPROFILE_GAMERCARD_ZONE
constexpr const char* GamerCardZoneName(int32_t zone) {
  switch (zone) {
    case 1:
      return "Recreation";
    case 2:
      return "Pro";
    case 3:
      return "Family";
    case 4:
      return "Underground";
    default:
      return nullptr;
  }
}

inline std::string FormatScore(int64_t value) { return std::to_string(value); }

inline std::string FormatProgress(int earned, int total) {
  return std::to_string(earned) + " / " + std::to_string(total);
}

inline GamerCardView BuildGamerCardView(const GamerCardRequest& request, const GamerCardSources& sources) {
  GamerCardView view;
  view.game = kGamerCardGame;
  if (!IsLocalGamerCard(request, sources)) {
    view.status = "Profile unavailable";
    return view;
  }
  view.available = true;
  view.gamertag = sources.names[request.user_index];
  view.status = "Local profile";

  // The achievements, scores and profile settings belong to the console profile, which player 1 plays on.
  if (request.user_index != 0) return view;

  if (sources.achievements) {
    const AchievementSummary& a = *sources.achievements;
    view.profile_rows.push_back({"Gamerscore", FormatProgress(a.earned_gamerscore, a.total_gamerscore)});
    view.profile_rows.push_back({"Achievements", FormatProgress(a.unlocked, a.total)});
  }
  if (!sources.motto.empty()) view.profile_rows.push_back({"Motto", sources.motto});
  if (const char* zone = GamerCardZoneName(sources.zone)) view.profile_rows.push_back({"Zone", zone});
  if (sources.region > 0) view.profile_rows.push_back({"Region", std::to_string(sources.region)});

  if (sources.story_score) view.game_rows.push_back({"Story Score", FormatScore(*sources.story_score)});
  if (sources.survival_score) view.game_rows.push_back({"Survival Score", FormatScore(*sources.survival_score)});
  return view;
}

}  // namespace tmnt
