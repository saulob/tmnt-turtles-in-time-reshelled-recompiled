#pragma once

#include <array>
#include <cstddef>

#include "online/snes_features.h"
#include "snes_gameplay/gameplay_data.h"
#include "snes_gameplay/session.h"

namespace tmnt::snes {

inline constexpr char kMenuTitle[] = "SNES";
inline constexpr char kMenuAudioSection[] = "AUDIO";
inline constexpr char kMenuGameplaySection[] = "GAMEPLAY";
inline constexpr char kMenuCloseHint[] = "F9 Close";
inline constexpr char kMenuNote[] = "Not saved to the launcher";
inline constexpr char kMenuOnlineNote[] = "Online: each player's gameplay choices apply to their own turtle";
inline constexpr char kMenuNextMatchNote[] = "Applies to the next online match";
inline constexpr char kMenuNotImported[] = "SNES ROM not imported";
inline constexpr char kMenuReimport[] = "SNES gameplay data unavailable: re-import the ROM in the launcher";
inline constexpr char kMenuUnsupportedRom[] = "SNES gameplay data unavailable: unsupported ROM version";
inline constexpr char kMenuSoundtrackUnavailable[] = "SNES soundtrack: Unavailable";

inline constexpr size_t kMenuRowCount = kFeatureCount;
inline constexpr size_t kMenuAudioRow = 0;
inline constexpr size_t kMenuGameplayRow = 1;

inline const FeatureInfo& MenuRow(size_t row) { return kFeatures[row]; }

// What the running game has to offer. The soundtrack and the gameplay data come from the same import but are checked
// apart: an older import keeps a usable soundtrack and asks for a new import only for the gameplay data.
struct Availability {
  bool audio = false;
  GameplayStatus gameplay = GameplayStatus::kNotImported;
};

// The online negotiation of the gameplay rows as the F9 menu shows it. `online` is true while the party has other players
// or a match runs; `locked`, from the game's session start, means the running match no longer follows those rows.
struct OnlineView {
  bool online = false;
  bool locked = false;
  std::array<online::SnesStatus, online::kSnesFeatureCount> status{};
  bool dash_manual_available = true;
};

// Why an online gameplay row cannot be used; null when it can.
inline constexpr char kMenuManualDashUnavailable[] = "Unavailable: the online service does not support Manual Dash";

inline const char* OnlineReason(online::SnesStatus status) {
  switch (status) {
    case online::SnesStatus::kAvailable:
    case online::SnesStatus::kUnsupportedLocally:
      return nullptr;
    case online::SnesStatus::kUnsupportedByOther: return "Unavailable: another player does not support this feature";
    case online::SnesStatus::kUnsupportedByService: return "Unavailable: the online service does not support this feature";
    case online::SnesStatus::kWaiting: return "Waiting for players";
    case online::SnesStatus::kIncompatible: return "Incompatible SNES data";
  }
  return nullptr;
}

// The line under the gameplay rows: a running online match is not changed by them.
inline const char* OnlineNote(const OnlineView& view) { return view.online && view.locked ? kMenuNextMatchNote : kMenuOnlineNote; }

struct RowState {
  const char* text;
  bool lit;
  bool usable;
  const char* reason;  // shown under the row; may be null
};

// Classic Soundtrack is local and shows its own On or Off wherever this PC has the import. Online, a gameplay row is the
// player's own choice for their own turtle, usable only while every player can run the mechanic (or before they are all
// known); an opponent's choice never changes it.
inline RowState RowStateOf(const OnlineView& view, Feature feature, bool available, bool on) {
  const RowState local{on ? "On" : "Off", available && on, available, nullptr};
  if (!view.online || !InfoOf(feature).gameplay || !available) return local;
  const online::SnesStatus status = view.status[static_cast<size_t>(*OnlineFeature(feature))];
  if (status == online::SnesStatus::kAvailable) return local;
  if (status == online::SnesStatus::kWaiting) return {local.text, local.lit, true, OnlineReason(status)};
  return {"Unavailable", false, false, OnlineReason(status)};
}

// The Dash row is a mode: it shows the mode itself, and an unusable Manual online stays selectable so it can be left.
inline RowState DashRowState(const OnlineView& view, bool available, DashMode mode) {
  RowState state = RowStateOf(view, Feature::kDash, available, mode != DashMode::kOff);
  if (!available) return state;
  if (state.usable) state.text = DashLabel(mode);
  if (view.online && state.usable && mode == DashMode::kManual && !view.dash_manual_available) {
    state.lit = false;
    state.reason = kMenuManualDashUnavailable;
  }
  return state;
}

inline bool RowAvailable(const Availability& available, Feature feature) {
  return InfoOf(feature).gameplay ? available.gameplay == GameplayStatus::kReady : available.audio;
}

// The one line that explains a disabled row; null when every row is available.
inline const char* StatusMessage(const Availability& available) {
  switch (available.gameplay) {
    case GameplayStatus::kNotImported:
      return kMenuNotImported;
    case GameplayStatus::kReimportRequired:
      return kMenuReimport;
    case GameplayStatus::kUnsupportedRom:
      return kMenuUnsupportedRom;
    case GameplayStatus::kReady:
      break;
  }
  return available.audio ? nullptr : kMenuSoundtrackUnavailable;
}

enum class MenuKey { kF9, kEscape, kPadB, kUp, kDown, kEnter, kPadA, kLeft, kRight };

// The menu exists only once the game runs, and never over another overlay.
inline bool CanOpenMenu(bool attached, bool modal_open) { return attached && !modal_open; }

// What F9, Escape and the pad do to the menu. F9 opens or closes it and changes nothing else; a change applies to the
// session only.
class MenuModel {
 public:
  enum class Action { kNone, kOpen, kClose, kChange };
  struct Result {
    Action action = Action::kNone;
    size_t row = 0;
    int step = 1;  // Dash only: -1 goes back through Off, Auto, Manual
  };

  bool open() const { return open_; }
  size_t selected() const { return selected_; }
  void Select(size_t row) {
    if (open_ && row < kMenuRowCount) selected_ = row;
  }

  // The dialog went away by itself, as the launcher's overlays do.
  void Closed() { open_ = false; }

  Result Press(MenuKey key, bool can_open, const Availability& available) {
    if (key == MenuKey::kF9) {
      if (open_) return Close();
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
        selected_ = (selected_ + kMenuRowCount - 1) % kMenuRowCount;
        return {};
      case MenuKey::kDown:
        selected_ = (selected_ + 1) % kMenuRowCount;
        return {};
      case MenuKey::kEnter:
      case MenuKey::kPadA:
        return RowAvailable(available, MenuRow(selected_).feature) ? Result{Action::kChange, selected_} : Result{};
      case MenuKey::kLeft:
      case MenuKey::kRight:
        if (MenuRow(selected_).feature != Feature::kDash || !RowAvailable(available, Feature::kDash)) return {};
        return {Action::kChange, selected_, key == MenuKey::kLeft ? -1 : 1};
      case MenuKey::kF9:
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

}  // namespace tmnt::snes
