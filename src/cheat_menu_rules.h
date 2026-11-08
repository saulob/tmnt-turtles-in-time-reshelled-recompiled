#pragma once

#include <array>
#include <cstddef>

#include "cheats.h"

namespace tmnt::cheats {

inline constexpr char kCheatMenuTitle[] = "CHEATS";
inline constexpr char kCheatMenuCloseHint[] = "F10 Close";
inline constexpr char kCheatMenuNote[] = "Not saved to the launcher";
inline constexpr char kCheatMenuOnlineNote[] = "No effect in online matches";
inline constexpr char kCheatsOnlineUnavailable[] = "Cheats are unavailable while Online play is enabled.";

inline constexpr char kSpinAttackControllerHelp[] = "Controller: B";
inline constexpr char kSpinAttackKeyboardHelp[] = "Keyboard:";

struct CheatMenuRow {
  const char* label;
  bool multiplier;  // cycles Off, 2x, 4x instead of toggling
  Cheat cheat;
};

inline constexpr std::array<CheatMenuRow, 7> kCheatMenuRows{{
    {"Infinite Health", false, Cheat::Health},
    {"Infinite Lives", false, Cheat::Lives},
    {"Damage Multiplier", true, Cheat::Health},
    {"Turbo Attack", false, Cheat::Turbo},
    {"Turbo Strong Attack", false, Cheat::TurboStrong},
    {"Super Armor", false, Cheat::SuperArmor},
    {"Spin Attack", false, Cheat::SpinAttack},
}};
inline constexpr size_t kCheatMenuRowCount = kCheatMenuRows.size();
inline constexpr size_t kCheatMenuPlayerRow = 0;
inline constexpr size_t kCheatMenuCombatRow = 2;

inline const char* DamageMultiplierLabel(int multiplier) {
  return multiplier == 4 ? "4x" : (multiplier == 2 ? "2x" : "Off");
}

enum class MenuKey { kF10, kEscape, kPadB, kUp, kDown, kEnter, kPadA };

// The menu exists only once the game runs, and never over another overlay.
inline bool CanOpenCheatMenu(bool attached, bool modal_open) { return attached && !modal_open; }

// What F10, Escape and the pad do to the menu. A change applies to the session only.
class CheatMenuModel {
 public:
  enum class Action { kNone, kOpen, kClose, kChange, kUnavailable };
  struct Result {
    Action action = Action::kNone;
    size_t row = 0;
  };

  bool open() const { return open_; }
  size_t selected() const { return selected_; }
  void Select(size_t row) {
    if (open_ && row < kCheatMenuRowCount) selected_ = row;
  }

  // The dialog went away by itself, as the launcher's overlays do.
  void Closed() { open_ = false; }

  // Online, F10 only says that cheats are unavailable, and an open menu changes nothing.
  Result Press(MenuKey key, bool can_open, bool online = false) {
    if (key == MenuKey::kF10) {
      if (open_) return Close();
      if (online) return {Action::kUnavailable, 0};
      if (!can_open) return {};
      open_ = true;
      selected_ = 0;
      return {Action::kOpen, 0};
    }
    if (!open_) return {};
    switch (key) {
      case MenuKey::kEscape:
      case MenuKey::kPadB:
        return Close();
      case MenuKey::kUp:
        selected_ = (selected_ + kCheatMenuRowCount - 1) % kCheatMenuRowCount;
        return {};
      case MenuKey::kDown:
        selected_ = (selected_ + 1) % kCheatMenuRowCount;
        return {};
      case MenuKey::kEnter:
      case MenuKey::kPadA:
        return online ? Result{} : Result{Action::kChange, selected_};
      case MenuKey::kF10:
        break;
    }
    return {};
  }

 private:
  Result Close() {
    open_ = false;
    return {Action::kClose, selected_};
  }

  bool open_ = false;
  size_t selected_ = 0;
};

}  // namespace tmnt::cheats
