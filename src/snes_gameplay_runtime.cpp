#include "snes_gameplay_runtime.h"

#include <atomic>
#include <optional>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "classic_soundtrack_runtime.h"
#include "online_runtime.h"
#include "settings.h"
#include "snes_gameplay/online_rules.h"
#include "snes_gameplay/runtime_start.h"

REXCVAR_DEFINE_STRING(tmnt_snes_gameplay_data, "", "TMNT",
                      "Written at start from the imported SNES ROM for the game module; never saved");
REXCVAR_DEFINE_UINT32(tmnt_snes_session, 0, "TMNT",
                      "SNES overrides of the in-game F9 menu for this session; not a setting, never saved");
REXCVAR_DEFINE_UINT32(tmnt_snes_online_lock, 0, "TMNT",
                      "SNES features locked for the running online match, written by the exe for the game module; "
                      "never saved");

namespace tmnt {
namespace {

snes::GameplayStatus g_gameplay_status = snes::GameplayStatus::kNotImported;
std::optional<snes::RuntimeData> g_runtime_data;
std::atomic<uint32_t> g_published_lock{0};

bool SavedValue(snes::Feature feature) { return rex::cvar::Query<bool>(snes::InfoOf(feature).setting); }

void PublishLock(uint32_t lock) {
  if (g_published_lock.exchange(lock) == lock) {
    return;
  }
  rex::cvar::SetFlagByName(snes::kOnlineLockSetting, std::to_string(lock));
  REXLOG_INFO("SNES: online gameplay lock {}", lock ? "set, mechanics " + std::to_string(lock & 7) : "released");
}

}  // namespace

void StartSnesGameplay(const std::filesystem::path& user_data_root) {
  const snes::GameplayStatusResult status = snes::QueryGameplayStatus(user_data_root);
  g_gameplay_status = status.status;
  const snes::RuntimeStart start = snes::PrepareRuntimeStart(status);
  if (!start.data.empty()) {
    g_runtime_data = snes::ToRuntimeData(status.data);
  }
  rex::cvar::SetFlagByName(snes::kRuntimeDataSetting, start.data);
  if (start.warning) {
    REXLOG_WARN("{}", start.message);
  } else {
    REXLOG_INFO("{}", start.message);
  }
  if (!start.data.empty()) {
    REXLOG_INFO("SNES Gameplay: directional combat {}, throw control {}, dash {}",
                REXCVAR_GET(tmnt_snes_directional_combat) ? "On" : "Off",
                REXCVAR_GET(tmnt_snes_throw_control) ? "On" : "Off", REXCVAR_GET(tmnt_snes_dash) ? "On" : "Off");
  }
}

snes::Availability SnesAvailability() { return {IsClassicSoundtrackReady(), g_gameplay_status}; }

bool SnesFeatureEnabled(snes::Feature feature) {
  return snes::EffectiveFeature(REXCVAR_GET(tmnt_snes_session), feature, SavedValue(feature));
}

online::SnesGameplayCapabilities LocalSnesCapabilities() {
  online::SnesGameplayCapabilities caps;
  for (const snes::FeatureInfo& info : snes::kFeatures) {
    const std::optional<online::SnesFeature> feature = snes::OnlineFeature(info.feature);
    if (!feature) {
      continue;
    }
    const uint32_t compat = g_runtime_data ? snes::GameplayCompat(info.feature, *g_runtime_data) : 0;
    const bool supported = snes::RowAvailable(SnesAvailability(), info.feature) && compat != 0;
    caps.supports.Set(*feature, supported);
    caps.enabled.Set(*feature, supported && SnesFeatureEnabled(info.feature));
    caps.compat[size_t(*feature)] = supported ? compat : 0;
  }
  return caps.Normalized();
}

void UpdateSnesOnline() {
  online::OnlineService::SnesNegotiation negotiation;
  if (OnlineRunning()) {
    negotiation = Online().Snes();
  }
  std::optional<online::SnesGameplaySet> own;
  if (negotiation.locked) {
    own = negotiation.locked->For(negotiation.my_slot);
  }
  PublishLock(snes::EncodeOnlineLock(own));
}

snes::OnlineView SnesOnline() {
  snes::OnlineView view;
  if (!OnlineRunning()) {
    return view;
  }
  const online::OnlineService::SnesNegotiation negotiation = Online().Snes();
  view.online = negotiation.online;
  view.locked = negotiation.locked.has_value();
  view.status = negotiation.status;
  return view;
}

void ToggleSnesFeatureForSession(snes::Feature feature) {
  if (!snes::RowAvailable(SnesAvailability(), feature)) {
    return;
  }
  const uint32_t next = snes::ToggledOverride(REXCVAR_GET(tmnt_snes_session), feature, SavedValue(feature));
  const bool enabled = snes::EffectiveFeature(next, feature, SavedValue(feature));
  if (feature == snes::Feature::kClassicSoundtrack && !SetClassicSoundtrackActive(enabled)) {
    return;
  }
  rex::cvar::SetFlagByName(snes::kSessionSetting, std::to_string(next));
  REXLOG_INFO("SNES: {} {} for this session", snes::InfoOf(feature).label, enabled ? "On" : "Off");
}

}  // namespace tmnt
