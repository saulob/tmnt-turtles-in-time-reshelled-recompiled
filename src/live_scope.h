#pragma once

#include <cstdint>

namespace tmnt::live {

// While a scope is open the local profile counts as signed in to Xbox Live; everything else sees the real state.
inline thread_local int t_live_scope = 0;
inline thread_local const char* t_live_scope_name = nullptr;

inline bool LiveScopeActive() { return t_live_scope > 0; }

class LiveScope {
 public:
  explicit LiveScope(const char* name) : outer_name_(t_live_scope_name) {
    ++t_live_scope;
    t_live_scope_name = name;
  }
  ~LiveScope() {
    --t_live_scope;
    t_live_scope_name = outer_name_;
  }
  LiveScope(const LiveScope&) = delete;
  LiveScope& operator=(const LiveScope&) = delete;

 private:
  const char* outer_name_;
};

inline constexpr int32_t kLocalControllers = 4;

constexpr bool IsLocalController(int32_t controller) { return controller >= 0 && controller < kLocalControllers; }

// The local profile needs no Xbox Live account or sign-in state to qualify inside a scope.
inline bool LocalProfileAccepted(int32_t controller) { return LiveScopeActive() && IsLocalController(controller); }

// Session and lobby code never runs inside a scope, even when it is reached from one.
class LiveScopeSuspended {
 public:
  LiveScopeSuspended() : depth_(t_live_scope) { t_live_scope = 0; }
  ~LiveScopeSuspended() { t_live_scope = depth_; }
  LiveScopeSuspended(const LiveScopeSuspended&) = delete;
  LiveScopeSuspended& operator=(const LiveScopeSuspended&) = delete;

 private:
  int depth_;
};

inline constexpr uint32_t kNoImport = 0xFFFFFFFF;
inline thread_local uint32_t t_forced_import = kNoImport;

// While open, the engine import table lookup for `index` yields a function that returns true.
class ForcedImport {
 public:
  explicit ForcedImport(uint32_t index) : outer_(t_forced_import) { t_forced_import = index; }
  ~ForcedImport() { t_forced_import = outer_; }
  ForcedImport(const ForcedImport&) = delete;
  ForcedImport& operator=(const ForcedImport&) = delete;

 private:
  uint32_t outer_;
};

inline bool ImportForced(uint32_t index) { return t_forced_import == index; }

// FireCommunicator_OnReceiveEvent(this, var): var.i32_1 is the event, var.i32_3 the proceed source.
inline constexpr uint32_t kFireEventSpInit = 127;
inline constexpr uint32_t kFireEventProceedSignal = 147;
inline constexpr uint32_t kProceedSourceLeaderboard = 5;
inline constexpr uint32_t kProceedSourceMpLocal = 7;
inline constexpr uint32_t kFireEventSourceOffset = 8;
inline constexpr uint32_t kFireEventControllerOffset = 4;
inline constexpr uint32_t kProceedSourceSinglePlayer = 2;
inline constexpr uint32_t kProceedSourceLast = 8;

// The two proceeds that raise LOC_WARNING_SIGN_IN: Single Player, and Press START, which is every source without a case.
// The sign-in change check treats a new owner as a sign-out, so nothing reached with players in game may claim.
constexpr bool ClaimsLocalProfile(uint32_t event, uint32_t source) {
  return event == kFireEventProceedSignal &&
         (source == kProceedSourceSinglePlayer || source == 0 || source > kProceedSourceLast);
}

// The three events whose check is the leaderboard sign-in or the "scores will not be updated" warning.
constexpr const char* LocalOnlineEventName(uint32_t event, uint32_t source) {
  if (event == kFireEventSpInit) return "REC_SP_INIT";
  if (event != kFireEventProceedSignal) return nullptr;
  if (source == kProceedSourceLeaderboard) return "PROCEED_LEADERBRD";
  if (source == kProceedSourceMpLocal) return "PROCEED_MP_LOCAL";
  return nullptr;
}

// LOC_NOTF_LEADERBRD_NOT_XLA, sent while a local player registers in game without a console sign-in.
inline constexpr uint32_t kNotificationLeaderboardNotXla = 16;
inline thread_local int t_local_player_join = 0;

class LocalPlayerJoin {
 public:
  LocalPlayerJoin() { ++t_local_player_join; }
  ~LocalPlayerJoin() { --t_local_player_join; }
  LocalPlayerJoin(const LocalPlayerJoin&) = delete;
  LocalPlayerJoin& operator=(const LocalPlayerJoin&) = delete;
};

inline bool DropsNotification(uint32_t id) { return t_local_player_join > 0 && id == kNotificationLeaderboardNotXla; }

// Engine import indices used by the compiled scripts (sub_87520018).
inline constexpr uint32_t kImportPlayerSignedIntoConsole = 1875;

}  // namespace tmnt::live
