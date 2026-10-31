#include "gamer_card.h"

#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/string/utf8.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "achievement_list.h"
#include "live_fix.h"
#include "player_names.h"

namespace tmnt {
namespace {

constexpr uint32_t kProfileZone = 0x10040004;
constexpr uint32_t kProfileRegion = 0x10040005;
constexpr uint32_t kProfileMotto = 0x402C0011;

using rex::system::xam::UserProfile;

int32_t ReadInt32Setting(UserProfile& profile, uint32_t id) {
  UserProfile::Setting* setting = profile.GetSetting(id);
  if (!setting || !setting->is_set || setting->type != UserProfile::Setting::Type::INT32) return 0;
  return static_cast<UserProfile::Int32Setting*>(setting)->value;
}

std::string ReadMottoSetting(UserProfile& profile) {
  UserProfile::Setting* setting = profile.GetSetting(kProfileMotto);
  if (!setting || !setting->is_set || setting->type != UserProfile::Setting::Type::WSTRING) return {};
  return rex::string::to_utf8(static_cast<UserProfile::UnicodeSetting*>(setting)->value);
}

GamerCardBridge g_bridge;

}  // namespace

void InstallGamerCard(GamerCardBridge bridge) {
  g_bridge = std::move(bridge);
  REXLOG_INFO("[GamerCard] Local gamer card bridge installed");
}

GamerCardSources ReadGamerCardSources(rex::system::KernelState& kernel,
                                      const rex::system::AchievementManager& achievements) {
  GamerCardSources sources;
  UserProfile* profile = kernel.user_profile();
  if (!profile) return sources;

  sources.local_xuid = profile->xuid();
  for (uint32_t user = 0; user < kGamerCardUsers; ++user) {
    sources.names[user] = rex::cvar::GetFlagByName(kPlayerNames[user].sdk_key);
  }
  sources.achievements = Summarize(ListAchievements(achievements));
  sources.story_score = live::LocalRating(sources.local_xuid, kStoryScoreView);
  sources.survival_score = live::LocalRating(sources.local_xuid, kSurvivalScoreView);
  sources.motto = ReadMottoSetting(*profile);
  sources.zone = ReadInt32Setting(*profile, kProfileZone);
  sources.region = ReadInt32Setting(*profile, kProfileRegion);
  return sources;
}

}  // namespace tmnt

using namespace tmnt;

// sub_826EDB48 is the game's XamShowGamerCardUIForXUID(user, xuid) wrapper; ReXGlue stubs the import it calls.
REX_EXTERN(__imp__sub_826EDB48);
REX_HOOK_RAW(sub_826EDB48) {
  if (!g_bridge.open) {
    __imp__sub_826EDB48(ctx, base);
    return;
  }
  g_bridge.open({.user_index = ctx.r3.u32, .xuid = ctx.r4.u64});
  ctx.r3.u64 = 0;
}
