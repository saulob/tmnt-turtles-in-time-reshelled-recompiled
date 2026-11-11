#pragma once

#include "cheats.h"
#include "online_setting.h"
#include "trg/settings.h"

namespace tmnt::cheats {

inline bool OnlinePlayOn(const trg::SettingsStore& settings) {
  return settings.GetBool(kOnlineEnabledSetting, kDefaultOnlineEnabled);
}

// Online Play keeps every saved cheat Off; the changes stay pending until Save or Play.
inline bool TurnCheatsOff(trg::SettingsStore& settings) {
  bool changed = false;
  for (const CheatInfo& cheat : kCheats) {
    if (settings.GetBool(cheat.setting, false)) {
      settings.SetBool(cheat.setting, false);
      changed = true;
    }
  }
  if (settings.GetInt(kDamageMultiplierSetting, 0) != 0) {
    settings.Set(kDamageMultiplierSetting, "0");
    changed = true;
  }
  return changed;
}

inline bool KeepCheatsOffOnline(trg::SettingsStore& settings) {
  return OnlinePlayOn(settings) && TurnCheatsOff(settings);
}

}  // namespace tmnt::cheats
