#include "online/online_service.h"

#include <algorithm>
#include <exception>
#include <thread>

#include "online/net_addr.h"
#include "online/stun.h"
#include "online/websocket.h"

namespace tmnt::online {
namespace {

constexpr double kStepTimeoutSeconds = 15.0;
constexpr double kMaxRetrySeconds = 20.0;
constexpr double kFatalRetrySeconds = 60.0;
constexpr double kRefreshSeconds = 30.0;
constexpr double kNameSettleSeconds = 1.5;
constexpr double kLeaveFlushSeconds = 2.0;
constexpr double kNoticeSeconds = 12.0;
constexpr double kHostRefreshSeconds = 60.0;
constexpr double kHostRetrySeconds = 5.0;
constexpr double kSearchSeconds = 3.0;
constexpr double kSearchFreshSeconds = 1.0;
constexpr double kSearchTimeoutSeconds = 5.0;
constexpr double kTelemetryFlushSeconds = 15.0;
constexpr double kGameStartTimeoutSeconds = 30.0;
constexpr double kTelemetryAckSeconds = 20.0;
constexpr double kBeaconTimeoutMs = 3000;
constexpr int kRoomHelloRetries = 5;
constexpr double kAcceptedInviteSeconds = 60.0;
constexpr double kSearchIdleSeconds = 10.0;
constexpr double kIdentityRetrySeconds = 0.25;

bool Fatal(const std::string& code) {
  return code == "proto" || code == "bad_game" || code == "bad_guest" || code == "unauthorized";
}

const char* Flag(bool value) { return value ? "true" : "false"; }

}  // namespace

class OnlineService::Locked {
 public:
  explicit Locked(OnlineService& service) : service_(service) { service_.mutex_.lock(); }
  ~Locked() {
    std::optional<AcceptedInvite> invite;
    invite.swap(service_.announce_invite_);
    service_.mutex_.unlock();
    service_.FlushLog();
    if (invite && service_.config_.invite_accepted) service_.config_.invite_accepted(*invite);
  }
  Locked(const Locked&) = delete;
  Locked& operator=(const Locked&) = delete;

 private:
  OnlineService& service_;
};

struct OnlineService::TestState {
  std::mutex mutex;
  bool done = false;
  std::optional<ConnectionReport> report;  // none when the test stopped without one
  std::string stopped;
};

OnlineService::OnlineService(ServiceConfig config) : config_(std::move(config)) {
  if (!config_.test_beacon && !config_.synchronous_probes) {
    config_.test_beacon = [url = config_.url](const std::string& path) {
      std::thread([url, path] {
        HttpResult result;
        std::string error;
        http_get(url, path, int(kBeaconTimeoutMs), result, &error);
      }).detach();
    };
  }
  lobby_diag_ = Telemetry::NewDiagId();
  telemetry_context_ = MakeContext(lobby_diag_, 0, 0);
  telemetry_.SetContext(telemetry_context_);
}

TelemetryContext OnlineService::MakeContext(const std::string& diag, int slot, int party_size) const {
  TelemetryContext context;
  context.game_id = kGameId;
  context.diag = diag;
  context.port_version = config_.version;
  context.platform = config_.platform;
  context.os = config_.platform;
  context.arch = "x64";
  context.fingerprint = config_.fingerprint;
  context.protocol = kLobbyProtocol;
  context.slot = slot;
  context.host = slot == 1;
  context.party_size = party_size;
  return context;
}

OnlineService::~OnlineService() = default;

void OnlineService::Disable() {
  if (Connected() && channel_ && party_) SendLocked(EncodePartyLeave(next_seq_++));
  EndParty("disabled");
  if (Connected() && channel_) {
    leaving_ = std::move(channel_);
    leaving_deadline_ = now_ + kLeaveFlushSeconds;
  }
  if (channel_) channel_->Close();
  channel_.reset();
  step_ = Step::kIdle;
  pending_.clear();
  EndParty("disabled");
  friends_.clear();
  incoming_.clear();
  outgoing_.clear();
  hosting_.reset();
  hosting_session_.clear();
  StopPlayerMatchSearch();
  results_.clear();
  invites_.clear();
  DropAcceptedInviteLocked("disabled");
  if (joining_) join_refused_ = "link_lost";
  joining_ = false;
  friend_code_.clear();
  fetched_ = false;
  connection_ = Connection::kOff;
  connection_text_.clear();
  notice_.clear();
  notices_.clear();
  test_.reset();
  testing_ = false;
  test_needs_identity_ = false;
  test_probe_.reset();
  retry_at_ = 0;
  retry_delay_ = 1.0;
}

void OnlineService::Update(double now) {
  Locked lock(*this);
  now_ = now;
  if (leaving_) {
    ChannelEvent event;
    bool done = now >= leaving_deadline_;
    while (!done && leaving_->Poll(event)) done = event.kind != ChannelEvent::Kind::kOpen;
    if (done) {
      leaving_->Close();
      leaving_.reset();
    }
  }
  if (!settings_.enabled) return;
  if (test_needs_identity_) StartTest();
  CollectTest();
  if (step_ == Step::kIdle && !channel_ && now >= retry_at_) OpenChannel(now);
  PumpChannel(now);
  PumpRoom(now);
  if (session_) {
    session_->Update(now);
    if (session_->fatal()) {
      Notice("The connection to the host was lost.");
      if (Connected()) SendLocked(EncodePartyLeave(next_seq_++));
      EndParty("host_unreachable");
    }
  }
  UpdateGameStart(now);
  ExpireAcceptedInvite(now);
  const bool opening = step_ == Step::kOpening || step_ == Step::kGreeting;
  if (opening && now > step_deadline_) OnLinkLost("timeout", now);
  if (step_ != Step::kOnline) return;
  if (now >= next_refresh_) Refresh();
  PumpPlayerMatch();
  if (now >= next_telemetry_) {
    next_telemetry_ = now + kTelemetryFlushSeconds;
    FlushTelemetry(now);
  }
  if (SessionName() != greeted_name_ && !party_ && now - name_since_ >= kNameSettleSeconds) {
    Log("DEBUG_SAULO online name changed, signing in again");
    if (channel_) channel_->Close();
    channel_.reset();
    step_ = Step::kIdle;
    pending_.clear();
    OpenChannel(now);
  }
}

void OnlineService::OnEvent(const ChannelEvent& event, double now) {
  switch (event.kind) {
    case ChannelEvent::Kind::kOpen:
      Log("DEBUG_SAULO online worker connected");
      step_ = Step::kGreeting;
      step_deadline_ = now + kStepTimeoutSeconds;
      Send(Purpose::kHello, [this](int seq) {
        return EncodeHello(seq, {config_.guest, config_.build, identity_.player_id, identity_.token, SessionName(),
                                 config_.version, config_.platform, true, config_.relay && config_.game_socket});
      });
      break;
    case ChannelEvent::Kind::kText:
      OnText(event.text, now);
      break;
    case ChannelEvent::Kind::kBinary:
      break;
    case ChannelEvent::Kind::kClosed:
      OnLinkLost(event.text.empty() ? "closed" : event.text, now);
      break;
  }
}

void OnlineService::OnResponse(const ServerMessage& message, double now) {
  const auto found = pending_.find(message.seq);
  if (found == pending_.end()) return;
  const Pending pending = found->second;
  pending_.erase(found);
  const bool ok = message.code == "ok";

  switch (pending.purpose) {
    case Purpose::kHello:
      if (!ok) {
        Log("DEBUG_SAULO online sign-in refused code=" + message.code);
        connection_ = Fatal(message.code) ? Connection::kError : Connection::kOffline;
        connection_text_ = DescribeServiceError(message.code);
        if (channel_) channel_->Close();
        channel_.reset();
        step_ = Step::kIdle;
        retry_at_ = now + (Fatal(message.code) || message.code == "limit" ? kFatalRetrySeconds : retry_delay_);
        retry_delay_ = std::min(retry_delay_ * 2, kMaxRetrySeconds);
        return;
      }
      OnGreeted(message, now);
      break;
    case Purpose::kFriends:
      if (ok && message.friends) {
        friends_ = *message.friends;
        fetched_ = true;
        Log("DEBUG_SAULO online friends count=" + std::to_string(friends_.size()));
      }
      break;
    case Purpose::kRequests:
      if (ok) {
        if (message.incoming) incoming_ = *message.incoming;
        if (message.outgoing) outgoing_ = *message.outgoing;
      }
      break;
    case Purpose::kAdd:
      if (!ok) {
        Notice(DescribeServiceError(message.code));
        Log("DEBUG_SAULO online friend request failed code=" + pending.player + " reason=" + message.code);
      } else {
        Notice(message.state == "friends" ? "You are now friends." : "Friend request sent.");
        Log("DEBUG_SAULO online friend request accepted by service code=" + pending.player + " state=" + message.state);
        Refresh();
      }
      break;
    case Purpose::kAccept:
    case Purpose::kDecline:
    case Purpose::kRemove:
      if (!ok) Notice(DescribeServiceError(message.code));
      Refresh();
      break;
    case Purpose::kPlayerMatchHost:
      if (!hosting_) break;
      if (!ok) {
        Log("DEBUG_SAULO player_match advertise failed code=" + message.code);
        if (hosting_session_.empty()) next_host_refresh_ = now_ + kHostRetrySeconds;
        break;
      }
      if (message.access) AdoptHostAccess(*message.access);
      if (!hosting_) break;
      if (message.session != hosting_session_) {
        hosting_session_ = message.session;
        Log("DEBUG_SAULO player_match advertise session_id=" + hosting_session_);
      }
      break;
    case Purpose::kPlayerMatchSearch:
      if (!searching_ || pending.search_epoch != search_epoch_) {
        Log("DEBUG_SAULO player_match search ignored stale");
        break;
      }
      if (!ok || !message.sessions) {
        Log("DEBUG_SAULO player_match search failed code=" + message.code);
        break;
      }
      if (message.sessions->size() != results_.size() || search_generation_ == 0) {
        Log("DEBUG_SAULO player_match search results=" + std::to_string(message.sessions->size()));
      }
      results_ = *message.sessions;
      ++search_generation_;
      search_answered_at_ = now_;
      break;
    case Purpose::kPlayerMatchJoin: {
      const bool abandoned = abandon_join_;
      joining_ = false;
      abandon_join_ = false;
      if (accepted_invite_ && accepted_invite_->session == pending.player) {
        DropAcceptedInviteLocked(ok ? "joined" : "join_rejected_" + message.code);
      }
      if (!ok) {
        join_refused_ = message.code;
        Notice(DescribeJoinError(message.code));
        Log("DEBUG_SAULO player_match join failed code=" + message.code);
        Emit({"join_failed", 0, {{"reason", message.code}}});
      } else if (abandoned) {
        Log("DEBUG_SAULO player_match join answered after the game left");
        if (message.access) Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
      } else {
        Log("DEBUG_SAULO player_match join complete session=" + pending.player);
        if (message.access && !party_) AdoptAccess(*message.access);
        Emit({"join_completed", 0, {}});
      }
      break;
    }
    case Purpose::kPartyCreate:
      if (!ok || !message.access) {
        Notice(DescribeServiceError(ok ? "bad_request" : message.code));
        Log("DEBUG_SAULO party create failed code=" + message.code);
        break;
      }
      AdoptAccess(*message.access);
      break;
    case Purpose::kInviteSend:
      if (!ok) {
        Notice(DescribeServiceError(message.code));
        Log("DEBUG_SAULO party invite failed code=" + message.code);
        if (message.code != "party_full") Emit({"invite_failed", 0, {{"reason", message.code}}});
      } else {
        Notice("Invite sent to " + NameOf(pending.player) + ".");
        Log("DEBUG_SAULO party invite sent");
      }
      break;
    case Purpose::kInviteAccept:
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(),
                                    [&](const Invite& i) { return i.id == pending.player; }),
                     invites_.end());
      if (!ok || !message.access) {
        Notice(DescribeJoinError(ok ? "bad_request" : message.code));
        Log("DEBUG_SAULO party invite accept failed code=" + message.code);
        if (accepted_invite_ && accepted_invite_->invite == pending.player) DropAcceptedInviteLocked("accept_rejected_" + message.code);
        if (message.code != "party_full") Emit({"join_failed", 0, {{"reason", message.code}}});
        break;
      }
      AdoptAccess(*message.access);
      Emit({"join_completed", 0, {}});
      if (accepted_invite_ && accepted_invite_->invite == pending.player) {
        accepted_invite_->ready = true;
        accepted_deadline_ = now + kAcceptedInviteSeconds;
        Log("DEBUG_SAULO accepted invite ready generation=" + std::to_string(accepted_invite_->generation));
        announce_invite_ = accepted_invite_;
      }
      break;
    case Purpose::kTelemetry:
      if (telemetry_inflight_.seq == message.seq) {
        if (message.code == "limit" || message.code == "internal") telemetry_.RestoreBatch(telemetry_inflight_.batch);
        telemetry_inflight_ = {};
      }
      break;
    case Purpose::kPartyLeave:
    case Purpose::kPartyKick:
    case Purpose::kInviteDecline:
    case Purpose::kPlayerMatchCancel:
    case Purpose::kRoomHello:
      if (!ok) Log("DEBUG_SAULO online request failed code=" + message.code);
      break;
  }
}

void OnlineService::OnGreeted(const ServerMessage& message, double now) {
  const bool was_reconnecting = connection_ == Connection::kChecking && !friend_code_.empty();
  step_ = Step::kOnline;
  connection_ = Connection::kOnline;
  connection_text_ = "Online";
  retry_delay_ = 1.0;
  friend_code_ = message.friend_code.empty() ? identity_.player_id : message.friend_code;
  greeted_name_ = SessionName();
  Log(std::string("DEBUG_SAULO online signed in") + (was_reconnecting ? " (reconnected)" : "") + " name=" + greeted_name_ +
      " code=" + friend_code_);
  capabilities_ = message.capabilities.value_or(ServiceCapabilities{});
  if (capabilities_.announced) {
    Log("DEBUG_SAULO online service capabilities protocol=" + std::to_string(capabilities_.protocol_version) +
        " worker_build=" + (capabilities_.worker_build.empty() ? std::string("-") : capabilities_.worker_build) +
        " public_endpoints=" + Flag(capabilities_.public_endpoints) + " relay_supported=" + Flag(capabilities_.relay_supported));
  } else {
    Log("DEBUG_SAULO online service capabilities protocol=legacy relay_supported=false");
  }
  if (was_reconnecting) Emit({"worker_reconnect", 0, {}});
  party_check_ = true;
  Refresh();
  if (hosting_ && !signaling_ended_) SendPlayerMatchHost();
  (void)now;
}

void OnlineService::OnServerEvent(const ServerMessage& message, double now) {
  using Kind = ServerMessage::Kind;
  (void)now;
  switch (message.kind) {
    case Kind::kFriendRequest:
      Notice(message.request.name + " wants to be your friend.");
      Log("DEBUG_SAULO online friend request received from=" + message.request.player);
      Refresh();
      break;
    case Kind::kFriendAdded:
      Notice(message.friend_view.name + " is now your friend.");
      UpdateFriend(message.friend_view);
      Refresh();
      break;
    case Kind::kFriendRemoved:
      friends_.erase(std::remove_if(friends_.begin(), friends_.end(),
                                    [&](const Friend& f) { return f.player == message.player; }),
                     friends_.end());
      Refresh();
      break;
    case Kind::kFriendStatus:
      UpdateFriend(message.friend_view);
      break;
    case Kind::kFriendOffline:
      for (Friend& f : friends_) {
        if (f.player == message.player) {
          f.online = false;
          f.state = FriendState::kOffline;
        }
      }
      break;
    case Kind::kInvite: {
      const Invite& invite = *message.invite;
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite.id; }),
                     invites_.end());
      invites_.push_back(invite);
      Notice(invite.from_name + " invited you to a party.");
      Log("DEBUG_SAULO party invite received");
      Emit({"invite_received", 0, {}});
      break;
    }
    case Kind::kInviteEnded: {
      const size_t before = invites_.size();
      invites_.erase(std::remove_if(invites_.begin(), invites_.end(),
                                    [&](const Invite& i) { return i.id == message.invite_id; }),
                     invites_.end());
      const std::string ended = DescribeInviteEnded(message.reason, invites_.size() != before);
      if (!ended.empty()) Notice(ended);
      if (accepted_invite_ && !accepted_invite_->ready && accepted_invite_->invite == message.invite_id) {
        DropAcceptedInviteLocked("invite_ended_" + message.reason);
      }
      Log("DEBUG_SAULO party invite ended reason=" + message.reason);
      break;
    }
    case Kind::kPartyUpdate:
      if (message.party) OnParty(*message.party);
      break;
    case Kind::kPartyEnded:
      if (party_ && party_->match == message.match_id) ServiceEndedParty(message.reason);
      break;
    default:
      break;
  }
}

void OnlineService::OnLinkLost(const std::string& why, double now) {
  Log("DEBUG_SAULO online worker disconnected reason=" + why);
  if (channel_) channel_->Close();
  channel_.reset();
  RestoreInflightTelemetry();
  pending_.clear();
  const bool was_online = step_ == Step::kOnline;
  step_ = Step::kIdle;
  hosting_session_.clear();
  if (joining_) join_refused_ = "link_lost";
  joining_ = false;
  invites_.clear();
  // A joined party lives on its room, which this link does not carry.
  if (accepted_invite_ && !accepted_invite_->ready) DropAcceptedInviteLocked("link_lost");
  for (Friend& f : friends_) {
    f.online = false;
    f.state = FriendState::kOffline;
  }
  if (was_online) Notice("Connection lost. Reconnecting...");
  const bool known = !friend_code_.empty();
  connection_ = known ? Connection::kChecking : Connection::kOffline;
  connection_text_ = known ? "Reconnecting" : DescribeServiceError("unreachable");
  retry_at_ = now + retry_delay_;
  retry_delay_ = std::min(retry_delay_ * 2, kMaxRetrySeconds);
}

void OnlineService::PumpPlayerMatch() {
  if (hosting_ && !signaling_ended_ && now_ >= next_host_refresh_) SendPlayerMatchHost();
  if (searching_ && search_from_game_ && now_ - search_asked_at_ >= kSearchIdleSeconds) {
    Log("DEBUG_SAULO player_match search ended: the game stopped asking");
    StopPlayerMatchSearch();
  }
  if (searching_ && now_ >= next_search_) SendPlayerMatchSearch();
}

void OnlineService::SendPlayerMatchHost() {
  next_host_refresh_ = now_ + kHostRefreshSeconds;
  const PlayerMatchRules rules = *hosting_;
  const std::string qos = hosting_qos_;
  const int slots = hosting_slots_;
  Send(Purpose::kPlayerMatchHost, [&](int seq) { return EncodePlayerMatchHost(seq, rules, qos, slots); });
}

void OnlineService::HostPlayerMatch(const PlayerMatchRules& rules, int slots) {
  Locked lock(*this);
  const bool fresh = !hosting_;
  hosting_ = rules;
  hosting_slots_ = std::clamp(slots, 2, kMaxParty);
  if (fresh) Log("DEBUG_SAULO player_match create begin slots=" + std::to_string(hosting_slots_));
  if (Connected()) SendPlayerMatchHost();
}

void OnlineService::CancelPlayerMatch() {
  Locked lock(*this);
  if (!hosting_) return;
  hosting_.reset();
  hosting_session_.clear();
  hosting_qos_.clear();
  Log("DEBUG_SAULO player_match advertisement removed");
  if (Connected()) Send(Purpose::kPlayerMatchCancel, [](int seq) { return EncodePlayerMatchCancel(seq); });
}

std::vector<PlayerMatchAd> OnlineService::PlayerMatchResults() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<PlayerMatchAd> results = results_;
  if (accepted_invite_ && accepted_invite_->ready &&
      std::none_of(results.begin(), results.end(), [this](const PlayerMatchAd& ad) { return ad.session == accepted_invite_->session; })) {
    PlayerMatchAd ad;
    ad.session = accepted_invite_->session;
    ad.player = accepted_invite_->host;
    ad.name = accepted_invite_->host_name;
    ad.slots = accepted_invite_->slots;
    results.push_back(ad);
  }
  return results;
}

bool OnlineService::JoinPlayerMatch(const std::string& session) {
  Locked lock(*this);
  const bool known = std::any_of(results_.begin(), results_.end(), [&](const PlayerMatchAd& ad) { return ad.session == session; }) ||
                     (accepted_invite_ && accepted_invite_->ready && accepted_invite_->session == session);
  if (!Connected() || !known || joining_) {
    join_refused_ = !Connected() ? "link_lost" : !known ? "not_found" : "busy";
    Log("DEBUG_SAULO player_match join refused");
    return false;
  }
  joining_ = true;
  abandon_join_ = false;
  join_refused_.clear();
  StopPlayerMatchSearch();
  Emit({"join_started", 0, {}});
  Log("DEBUG_SAULO player_match join begin");
  Send(Purpose::kPlayerMatchJoin, [&session](int seq) { return EncodePlayerMatchJoin(seq, session); }, session);
  return true;
}

OnlineService::PlayerMatchJoinAnswer OnlineService::JoinAnswer() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {joining_, join_refused_};
}

bool OnlineService::CreateParty() {
  Locked lock(*this);
  if (!Connected() || party_) return false;
  Send(Purpose::kPartyCreate, [](int seq) { return EncodePartyCreate(seq); });
  return true;
}

bool OnlineService::InviteToParty(const std::string& player) {
  Locked lock(*this);
  if (!Connected() || !party_ || !access_ || access_->slot != 1) {
    Notice("Start a party first.");
    return false;
  }
  if (hosting_ && hosting_session_.empty()) {
    Notice("The match is not ready yet.");
    return false;
  }
  Send(Purpose::kInviteSend, [&player](int seq) { return EncodeInviteSend(seq, player); }, player);
  return true;
}

bool OnlineService::AcceptPartyInvite(const std::string& invite) {
  Locked lock(*this);
  const bool known = std::any_of(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; });
  if (!Connected() || !known) return false;
  if (party_) {
    Notice("Leave your party before joining another.");
    return false;
  }
  if (accepted_invite_ && !accepted_invite_->ready) return false;
  const auto found = std::find_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; });
  DropAcceptedInviteLocked("replaced");
  if (!found->session.empty()) {
    accepted_invite_ = AcceptedInvite{++accepted_generation_, found->session, found->from_player, found->from_name, invite, false, found->slots};
    Log("DEBUG_SAULO accepted invite begin generation=" + std::to_string(accepted_generation_) + " session=" + found->session);
  }
  Emit({"join_started", 0, {}});
  Send(Purpose::kInviteAccept, [&invite](int seq) { return EncodeInviteAccept(seq, invite); }, invite);
  return true;
}

void OnlineService::DeclinePartyInvite(const std::string& invite) {
  Locked lock(*this);
  invites_.erase(std::remove_if(invites_.begin(), invites_.end(), [&](const Invite& i) { return i.id == invite; }),
                 invites_.end());
  if (Connected()) Send(Purpose::kInviteDecline, [&invite](int seq) { return EncodeInviteDecline(seq, invite); });
}

void OnlineService::KickFromParty(const std::string& player) {
  Locked lock(*this);
  if (Connected() && party_ && access_ && access_->slot == 1) {
    Send(Purpose::kPartyKick, [&player](int seq) { return EncodePartyKick(seq, player); });
  }
}

void OnlineService::LeaveParty() {
  Locked lock(*this);
  if (!party_) return;
  if (Connected()) Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
  EndParty("left");
}

void OnlineService::LeaveFinishedMatch() {
  Locked lock(*this);
  abandon_join_ = joining_;
  if (!party_) return;
  if (accepted_invite_ && accepted_invite_->ready) {
    Log("DEBUG_SAULO game left a session before joining its accepted invite; party kept");
    return;
  }
  Log("DEBUG_SAULO game left its session; leaving party");
  if (Connected()) Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
  EndParty("left");
}

std::optional<Party> OnlineService::party() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return party_;
}

int OnlineService::my_slot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return access_ ? access_->slot : 0;
}

std::optional<AcceptedInvite> OnlineService::AcceptedInviteInfo() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (accepted_invite_ && accepted_invite_->ready) return accepted_invite_;
  return std::nullopt;
}

void OnlineService::DropAcceptedInvite(const std::string& reason) {
  Locked lock(*this);
  DropAcceptedInviteLocked(reason);
}

void OnlineService::DropAcceptedInviteLocked(const std::string& reason) {
  if (!accepted_invite_) return;
  Log("DEBUG_SAULO accepted invite dropped generation=" + std::to_string(accepted_invite_->generation) + " reason=" + reason);
  accepted_invite_.reset();
}

// The party of an accepted invite is joined before the game is told; a game that never gets to it must not hold the seat.
void OnlineService::ExpireAcceptedInvite(double now) {
  if (!accepted_invite_ || !accepted_invite_->ready || joining_ || now < accepted_deadline_) return;
  DropAcceptedInviteLocked("join_timeout");
  Notice("The invited match could not be joined.");
  if (Connected()) Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
  EndParty("join_timeout");
}

std::vector<Invite> OnlineService::Invites() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return invites_;
}

void OnlineService::AdoptAccess(const PartyAccess& access) {
  access_ = access;
  Party party;
  if (access.party) {
    party = *access.party;
  } else {
    party.match = access.match;
    party.diag = access.diag;
    party.host = access.slot == 1 ? friend_code_ : std::string();
    party.generation = access.generation;
    party.members.push_back({access.slot, friend_code_, SessionName(), config_.platform, config_.version, MemberState::kJoined});
  }
  if (party.diag.empty()) party.diag = access.diag;
  Log("DEBUG_SAULO party joined slot=" + std::to_string(access.slot) + " diag=" + party.diag +
      " members=" + std::to_string(party.members.size()));
  party_ = party;
  party_check_ = false;
  room_retry_at_ = 0;
  room_retry_delay_ = 1.0;
  room_hello_failures_ = 0;
  FlushTelemetry(now_, true);
  telemetry_context_ = MakeContext(party.diag, access.slot, int(party.members.size()));
  telemetry_.SetContext(telemetry_context_, now_);
  party_started_ = now_;
}

// A host's advertisement made a party: this match's first, or a new one because the service lost the old party while
// this client was away.
void OnlineService::AdoptHostAccess(const PartyAccess& access) {
  if (!access_) {
    AdoptAccess(access);
    return;
  }
  if (access.match == access_->match) return;
  if (PlayingDirect()) {
    Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
    KeepPlayingWithoutService("party_replaced");
    return;
  }
  const std::optional<PlayerMatchRules> hosting = hosting_;
  const int slots = hosting_slots_;
  const std::string qos = hosting_qos_;
  EndParty("party_replaced");
  hosting_ = hosting;
  hosting_slots_ = slots;
  hosting_qos_ = qos;
  AdoptAccess(access);
}

void OnlineService::OnParty(const Party& party) {
  if (!access_) {
    if (!party_check_) return;
    party_check_ = false;
    Log("DEBUG_SAULO party left behind on the service; leaving it");
    Send(Purpose::kPartyLeave, [](int seq) { return EncodePartyLeave(seq); });
    return;
  }
  if (party.match != access_->match) return;
  if (!party.member(access_->slot)) {
    EndParty("removed");
    return;
  }
  party_ = party;
  telemetry_context_.party_size = int(party.members.size());
  Log("DEBUG_SAULO party update members=" + std::to_string(party.members.size()) + " invited=" +
      std::to_string(party.invited.size()));
}

void OnlineService::EndParty(const std::string& reason) {
  if (!party_ && !access_) return;
  Log("DEBUG_SAULO party ended reason=" + reason + (party_ ? " diag=" + party_->diag : std::string()));
  SessionReport report;
  report.duration_ms = int64_t((now_ - party_started_) * 1000.0);
  report.compatibility = "ok";
  report.disconnect_reason = reason;
  if (test_) {
    report.connection_test_usable = test_->overall_usable;
    report.worker_reachable = test_->worker_reachable;
    report.stun_ok = test_->stun_ok;
    report.direct_available = test_->direct_available;
    report.relay_available = test_->relay_available;
  }
  telemetry_.SetSession(report);
  Emit({"session_end", 0, {{"reason", reason}}});
  FinishGameStart(reason);
  CloseRoom();
  if (reason == "host_left") Notice("The host left the party.");
  else if (reason == "kicked" || reason == "removed") Notice("You were removed from the party.");
  else if (reason == "expired") Notice("The party expired.");
  DropAcceptedInviteLocked("party_ended_" + reason);
  hosting_.reset();
  hosting_session_.clear();
  hosting_qos_.clear();
  access_.reset();
  party_.reset();
  party_check_ = false;
  signaling_ended_ = false;
  FlushTelemetry(now_, true);
  telemetry_.ClearReports();
  telemetry_context_ = MakeContext(lobby_diag_, 0, 0);
  telemetry_.SetContext(telemetry_context_, now_);
}

void OnlineService::ServiceEndedParty(const std::string& reason) {
  if (reason != "kicked" && PlayingDirect()) KeepPlayingWithoutService(reason);
  else EndParty(reason);
}

bool OnlineService::PlayingDirect() const {
  if (!session_) return false;
  const std::vector<PeerView> peers = session_->peers();
  return std::any_of(peers.begin(), peers.end(), [](const PeerView& peer) {
    return peer.transport == MatchTransport::kDirect && (peer.link == NetplayLink::kConnected || peer.link == NetplayLink::kFighting);
  });
}

// Peers keep talking over the UDP links they proved; only the room goes away. The party ends with the game's session.
void OnlineService::KeepPlayingWithoutService(const std::string& reason) {
  if (signaling_ended_) return;
  signaling_ended_ = true;
  Log("DEBUG_SAULO party ended on the service while playing directly reason=" + reason);
  if (room_) room_->Close();
  room_.reset();
  room_step_ = RoomStep::kNone;
  if (session_) session_->OnRelayClosed(0, "service_ended", now_);
}

void OnlineService::Emit(const SessionEvent& event) {
  const std::string slot = std::to_string(event.slot);
  std::vector<Telemetry::Field> fields;
  for (const auto& [key, value] : event.fields) fields.emplace_back(key, value);
  if (event.slot) fields.emplace_back("slot", slot);
  telemetry_.Event(event.name, fields, now_);
  if (config_.event) config_.event(event);
}

void OnlineService::FlushTelemetry(double now, bool force) {
  if (!Connected()) return;
  if (telemetry_inflight_.seq && now - telemetry_inflight_.sent_at > kTelemetryAckSeconds) RestoreInflightTelemetry();
  if (telemetry_inflight_.seq && !force) return;
  if (session_) {
    for (const PeerView& peer : session_->peers()) {
      PeerReport report;
      report.local_slot = access_ ? access_->slot : 0;
      report.remote_slot = peer.slot;
      report.state = peer.link == NetplayLink::kNone && peer.transport != MatchTransport::kDirect &&
                             peer.transport != MatchTransport::kRelay
                         ? (peer.failure.empty() ? "connecting" : "lost")
                         : "connected";
      report.transport = peer.transport == MatchTransport::kDirect ? "direct"
                         : peer.transport == MatchTransport::kRelay ? "relay"
                         : peer.transport == MatchTransport::kFailed ? "failed" : "pending";
      report.direct_result = peer.direct_result;
      report.relay_result = peer.relay_result;
      report.fallback_reason = peer.fallback_reason;
      report.establishment_ms = peer.establishment_ms;
      report.ping = peer.ping;
      report.sent_packets = peer.relay_used ? peer.traffic.relay_tx_packets : peer.traffic.udp_tx_packets;
      report.received_packets = peer.relay_used ? peer.traffic.relay_rx_packets : peer.traffic.udp_rx_packets;
      report.disconnect_reason = peer.failure;
      telemetry_.SetPeer(report);
    }
  }
  const std::optional<std::string> batch = telemetry_.TakeBatch(now, force);
  if (!batch || batch->size() < 2) return;
  const int seq = next_seq_++;
  std::string text = *batch;
  text.insert(1, "\"seq\":" + std::to_string(seq) + ",");
  pending_[seq] = {Purpose::kTelemetry, {}, 0, now_};
  telemetry_inflight_ = {seq, *batch, now};
  SendLocked(text);
}

void OnlineService::RestoreInflightTelemetry() {
  if (!telemetry_inflight_.seq) return;
  telemetry_.RestoreBatch(telemetry_inflight_.batch);
  telemetry_inflight_ = {};
}

void OnlineService::NoteGameStart() {
  Locked lock(*this);
  if (!session_ || !party_ || game_start_.active) return;
  game_start_ = {true, now_};
  const std::vector<PeerView> peers = session_->peers();
  const long ready = std::count_if(peers.begin(), peers.end(), [](const PeerView& peer) {
    return peer.transport == MatchTransport::kDirect || peer.transport == MatchTransport::kRelay;
  });
  Emit({"game_start_attempt", 0, {{"members", std::to_string(party_->members.size())}, {"ready", std::to_string(ready)}}});
  Log("DEBUG_SAULO game start attempt members=" + std::to_string(party_->members.size()) + " ready=" + std::to_string(ready));
}

void OnlineService::UpdateGameStart(double now) {
  if (!game_start_.active || !session_) return;
  const std::vector<PeerView> peers = session_->peers();
  const int ms = int((now - game_start_.since) * 1000.0);
  const bool all_connected = !peers.empty() && std::all_of(peers.begin(), peers.end(), [](const PeerView& peer) {
    return peer.link == NetplayLink::kConnected || peer.link == NetplayLink::kFighting;
  });
  if (all_connected) {
    game_start_.active = false;
    Emit({"game_start_success", 0, {{"ms", std::to_string(ms)}}});
    Log("DEBUG_SAULO game start success ms=" + std::to_string(ms));
    return;
  }
  const auto failed = std::find_if(peers.begin(), peers.end(), [](const PeerView& peer) { return !peer.failure.empty(); });
  if (failed != peers.end()) {
    game_start_.active = false;
    Emit({"game_start_failure", 0, {{"reason", failed->failure}, {"slot", std::to_string(failed->slot)}, {"ms", std::to_string(ms)}}});
    Log("DEBUG_SAULO game start failure peer=" + std::to_string(failed->slot) + " reason=" + failed->failure);
  } else if (now - game_start_.since > kGameStartTimeoutSeconds) {
    game_start_.active = false;
    Emit({"game_start_failure", 0, {{"reason", "timeout"}, {"ms", std::to_string(ms)}}});
    Log("DEBUG_SAULO game start failure reason=timeout");
  }
}

void OnlineService::FinishGameStart(const std::string& reason) {
  if (!game_start_.active) return;
  game_start_.active = false;
  Emit({"game_start_failure", 0, {{"reason", reason}, {"ms", std::to_string(int((now_ - game_start_.since) * 1000.0))}}});
  Log("DEBUG_SAULO game start failure reason=" + reason);
}

void OnlineService::Refresh() {
  if (!Connected()) return;
  next_refresh_ = now_ + kRefreshSeconds;
  Send(Purpose::kFriends, [](int seq) { return EncodeFriendsList(seq); });
  Send(Purpose::kRequests, [](int seq) { return EncodeFriendsRequests(seq); });
}

std::vector<ActivePeer> OnlineService::Peers() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ActivePeer> out;
  if (!party_ || !access_) return out;
  for (const PartyMember& member : party_->members) {
    if (member.slot == access_->slot) continue;
    out.push_back({party_->match, member.slot, member.player, member.name, member.slot == 1});
  }
  return out;
}

void OnlineService::SetPeerLink(int slot, NetplayLink link) {
  Locked lock(*this);
  if (session_) session_->SetPeerLink(slot, link);
}

std::optional<int> OnlineService::WorstRtt() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<int> worst;
  if (!session_) return worst;
  for (const PeerView& peer : session_->peers()) {
    if (peer.rtt_ms && (!worst || *peer.rtt_ms > *worst)) worst = peer.rtt_ms;
  }
  return worst;
}

std::vector<NetplayAssignment> OnlineService::Assignments() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_ ? session_->Assignments() : std::vector<NetplayAssignment>();
}

std::optional<NetplayAssignment> OnlineService::Assignment(int slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_ ? session_->Assignment(slot) : std::nullopt;
}

Snapshot OnlineService::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot s;
  s.connection = connection_;
  s.connection_text = connection_text_;
  s.friend_code = friend_code_;
  s.fetched = fetched_;
  s.friends = friends_;
  s.incoming = incoming_;
  s.outgoing = outgoing_;
  s.invites = invites_;
  s.player_match.hosting = hosting_.has_value();
  s.player_match.session = hosting_session_;
  s.player_match.searching = searching_;
  s.player_match.results = results_;
  s.player_match.search_generation = search_generation_;
  s.party = party_;
  s.my_slot = access_ ? access_->slot : 0;
  s.room_state = room_step_ == RoomStep::kOnline ? "online" : party_ ? "connecting" : "none";
  if (session_) s.peers = session_->peers();
  if (now_ - notice_at_ <= kNoticeSeconds) s.notice = notice_;
  s.testing = testing_;
  s.test = test_;
  s.tests_finished = tests_finished_;
  return s;
}

// ---- The match room of the party ----

namespace {

std::string RoomUrl(std::string base, const std::string& match) {
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/match/" + match;
}

}  // namespace

void OnlineService::OpenRoom(double now) {
  if (!access_) return;
  room_ = config_.open_channel(RoomUrl(config_.url, access_->match));
  room_step_ = RoomStep::kOpening;
  room_deadline_ = now + kStepTimeoutSeconds;
}

void OnlineService::PumpRoom(double now) {
  if (!access_) {
    if (room_) CloseRoom();
    return;
  }
  if (signaling_ended_) return;
  if (!room_ && now >= room_retry_at_) OpenRoom(now);
  ChannelEvent event;
  while (room_ && room_->Poll(event)) OnRoomEvent(event, now);
  const bool opening = room_step_ == RoomStep::kOpening || room_step_ == RoomStep::kGreeting;
  if (room_ && opening && now > room_deadline_) OnRoomLost("timeout", now);
}

void OnlineService::OnRoomEvent(const ChannelEvent& event, double now) {
  switch (event.kind) {
    case ChannelEvent::Kind::kOpen:
      room_step_ = RoomStep::kGreeting;
      room_deadline_ = now + kStepTimeoutSeconds;
      room_hello_seq_ = next_seq_++;
      if (room_) room_->Send(EncodeMatchHello(room_hello_seq_, access_->match, access_->slot, access_->join_token,
                                              room_generation_seen_));
      break;
    case ChannelEvent::Kind::kText: {
      const std::optional<ServerMessage> message = DecodeServerMessage(event.text);
      if (!message) {
        Log("DEBUG_SAULO room error malformed message");
        break;
      }
      if (message->kind == ServerMessage::Kind::kResponse) {
        if (room_step_ == RoomStep::kGreeting && message->seq == room_hello_seq_) {
          OnRoomHello(*message, now);
        } else if (message->code != "ok") {
          Log("DEBUG_SAULO room request failed code=" + message->code);
          if (session_) session_->OnRefused(message->seq, message->code, now);
        }
      } else {
        OnRoomMessage(*message, now);
      }
      break;
    }
    case ChannelEvent::Kind::kBinary:
      if (session_) session_->OnBinary(event.text);
      break;
    case ChannelEvent::Kind::kClosed:
      OnRoomLost(event.text.empty() ? "closed" : event.text, now);
      break;
  }
}

void OnlineService::OnRoomHello(const ServerMessage& message, double now) {
  if (message.code != "ok") {
    Log("DEBUG_SAULO room refused code=" + message.code);
    // The service may not have been told of this seat yet when the first hello arrives.
    if (message.code == "not_found" && ++room_hello_failures_ <= kRoomHelloRetries) {
      OnRoomLost("refused_" + message.code, now);
      return;
    }
    if (message.code == "unauthorized" || message.code == "not_found" || message.code == "party_full") {
      Notice(DescribeJoinError(message.code));
      ServiceEndedParty(message.code);
      return;
    }
    OnRoomLost("refused_" + message.code, now);
    return;
  }
  room_step_ = RoomStep::kOnline;
  // Only now: the room counts a relay frame from a connection it has not greeted against that connection.
  {
    std::lock_guard<std::mutex> lock(room_route_->mutex);
    room_route_->room = room_;
  }
  room_retry_delay_ = 1.0;
  room_hello_failures_ = 0;
  room_generation_seen_ = message.generation;
  room_members_ = message.room_members.value_or(std::vector<RoomMember>());
  Log("DEBUG_SAULO room joined generation=" + std::to_string(message.generation) +
      " relay=" + Flag(message.relay_token != 0) + " members=" + std::to_string(room_members_.size()));
  if (!session_) StartSession(message.relay_token, message.generation);
  if (session_) {
    session_->OnMembers(room_members_, now);
    session_->ResendEndpoint();
    session_->ResendReports();
  }
}

void OnlineService::StartSession(uint32_t relay_token, int generation) {
  if (!config_.game_socket || !party_) return;
  MatchSessionConfig session;
  session.match = access_->match;
  session.diag = party_->diag;
  session.my_slot = access_->slot;
  session.relay_token = capabilities_.announced && !capabilities_.relay_supported ? 0 : relay_token;
  session.generation = ++match_generation_;
  session.socket = config_.game_socket;
  session.lan = config_.lan;
  session.stun = config_.stun;
  session.resolve_stun = config_.resolve_stun;
  session.synchronous_probes = config_.synchronous_probes;
  session.service_forwards_public = !capabilities_.announced || capabilities_.public_endpoints;
  session.full_mesh = config_.full_mesh;
  session.log = [this](const std::string& line) { Log(line); };
  session.send_text = [this](std::string&& text) { SendRoom(text); };
  session.send_binary = [route = room_route_](std::vector<uint8_t>&& frame) {
    std::shared_ptr<LobbyChannel> channel;
    {
      std::lock_guard<std::mutex> lock(route->mutex);
      channel = route->room.lock();
    }
    return channel && channel->SendBinary(std::move(frame));
  };
  session.notice = [this](const std::string& text) { Notice(text); };
  session.event = [this](const SessionEvent& event) { Emit(event); };
  session_ = std::make_unique<MatchSession>(std::move(session));
  (void)generation;
}

void OnlineService::OnRoomMessage(const ServerMessage& message, double now) {
  using Kind = ServerMessage::Kind;
  switch (message.kind) {
    case Kind::kMatchMembers:
      if (message.room_members) {
        room_members_ = *message.room_members;
        if (session_) session_->OnMembers(room_members_, now);
      }
      break;
    case Kind::kMatchPeer:
      if (session_) session_->OnPeer(message.peer, now);
      break;
    case Kind::kMatchTransport:
      if (session_) session_->OnTransport(message.pair_a, message.pair_b, message.transport, message.pair_by, now);
      break;
    case Kind::kRelayClosed:
      if (session_) session_->OnRelayClosed(message.slot, message.reason, now);
      break;
    case Kind::kMatchPeerLost:
      if (session_) session_->OnPeerLost(message.slot);
      break;
    case Kind::kMatchPeerBack:
      break;
    case Kind::kMatchLeft:
      if (session_) session_->OnPeerLeft(message.slot, message.reason, now);
      break;
    case Kind::kMatchEnded:
      Log("DEBUG_SAULO room ended reason=" + message.reason);
      if (party_) ServiceEndedParty(message.reason.empty() ? "closed" : message.reason);
      break;
    default:
      break;
  }
}

void OnlineService::OnRoomLost(const std::string& why, double now) {
  Log("DEBUG_SAULO room disconnected reason=" + why);
  if (room_) room_->Close();
  room_.reset();
  room_step_ = RoomStep::kNone;
  // The relay rides this connection: a pair on it waits for the room, a pair still choosing goes on without it.
  if (session_) session_->OnRelayClosed(0, "service_disconnected", now);
  room_retry_at_ = now + room_retry_delay_;
  room_retry_delay_ = std::min(room_retry_delay_ * 2, kMaxRetrySeconds);
  if (access_ && session_) Emit({"worker_reconnect", 0, {{"scope", "room"}}});
}

void OnlineService::CloseRoom() {
  if (room_ && room_step_ == RoomStep::kOnline) room_->Send(EncodeMatchLeave(next_seq_++));
  if (room_) room_->Close();
  room_.reset();
  room_step_ = RoomStep::kNone;
  session_.reset();
  room_members_.clear();
  room_generation_seen_ = 0;
}

void OnlineService::SendRoom(const std::string& text) {
  if (room_ && room_step_ == RoomStep::kOnline) room_->Send(text);
}

void OnlineService::Apply(const UserSettings& settings) {
  Locked lock(*this);
  const bool was_enabled = settings_.enabled;
  const std::string before = settings_.name;
  settings_ = settings;
  settings_.name = CleanOnlineName(settings.name);
  if (settings_.name != before) name_since_ = now_;
  if (was_enabled && !settings.enabled) Disable();
}

void OnlineService::SetGuest(const std::string& guest, int build) {
  Locked lock(*this);
  config_.guest = guest;
  config_.build = build;
}

void OnlineService::UpdateFriend(const Friend& entry) {
  for (Friend& f : friends_) {
    if (f.player == entry.player) {
      f = entry;
      return;
    }
  }
  friends_.push_back(entry);
}

void OnlineService::OpenChannel(double now) {
  now_ = now;
  if (!EnsureIdentity()) {
    if (connection_ != Connection::kChecking) {
      connection_ = Connection::kChecking;
      connection_text_ = "Connecting";
    }
    return;
  }
  if (!identity_.ok()) {
    connection_ = Connection::kError;
    connection_text_ = "Online identity is unavailable.";
    retry_at_ = now + kFatalRetrySeconds;
    return;
  }
  channel_ = config_.open_channel(config_.url);
  step_ = Step::kOpening;
  step_deadline_ = now + kStepTimeoutSeconds;
  if (connection_ != Connection::kChecking) {
    connection_ = Connection::kChecking;
    connection_text_ = "Connecting";
  }
}

void OnlineService::PumpChannel(double now) {
  ChannelEvent event;
  while (channel_ && channel_->Poll(event)) OnEvent(event, now);
}

void OnlineService::OnText(const std::string& text, double now) {
  const std::optional<ServerMessage> message = DecodeServerMessage(text);
  if (!message) {
    Log("DEBUG_SAULO online error malformed server message");
    return;
  }
  if (message->kind == ServerMessage::Kind::kResponse) OnResponse(*message, now);
  else OnServerEvent(*message, now);
}

FriendRequestResult OnlineService::AddFriend(const std::string& text) {
  Locked lock(*this);
  const std::string code = NormalizeFriendCode(text);
  const auto refuse = [&](FriendRequestResult result, const char* reason, const std::string& notice) {
    Log("DEBUG_SAULO online friend request failed code=" + (code.empty() ? std::string("-") : code) + " reason=" + reason);
    Notice(notice);
    return result;
  };
  if (!Connected()) return refuse(FriendRequestResult::kNotConnected, "not_connected", "Not connected to the online service.");
  if (code.empty()) return refuse(FriendRequestResult::kInvalid, "invalid", DescribeServiceError("bad_request"));
  if (code == friend_code_) return refuse(FriendRequestResult::kOwnCode, "own_code", "That is your own friend code.");
  const auto is_code = [&code](const auto& entry) { return entry.player == code; };
  if (std::any_of(friends_.begin(), friends_.end(), is_code)) {
    return refuse(FriendRequestResult::kAlreadyFriends, "already_friends", "Already friends.");
  }
  if (std::any_of(outgoing_.begin(), outgoing_.end(), is_code)) {
    return refuse(FriendRequestResult::kAlreadyPending, "already_pending", "Friend request already pending.");
  }
  Log("DEBUG_SAULO online friend request sent code=" + code);
  Send(Purpose::kAdd, [&code](int seq) { return EncodeFriendsAdd(seq, code); }, code);
  return FriendRequestResult::kSent;
}

void OnlineService::AcceptFriend(const std::string& player) {
  Locked lock(*this);
  incoming_.erase(std::remove_if(incoming_.begin(), incoming_.end(), [&](const Request& r) { return r.player == player; }),
                  incoming_.end());
  Send(Purpose::kAccept, [&player](int seq) { return EncodeFriendsAccept(seq, player); });
}

void OnlineService::DeclineFriend(const std::string& player) {
  Locked lock(*this);
  const auto drop = [&](std::vector<Request>& list) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Request& r) { return r.player == player; }), list.end());
  };
  drop(incoming_);
  drop(outgoing_);
  Send(Purpose::kDecline, [&player](int seq) { return EncodeFriendsDecline(seq, player); });
}

void OnlineService::RemoveFriend(const std::string& player) {
  Locked lock(*this);
  friends_.erase(std::remove_if(friends_.begin(), friends_.end(), [&](const Friend& f) { return f.player == player; }),
                 friends_.end());
  Send(Purpose::kRemove, [&player](int seq) { return EncodeFriendsRemove(seq, player); });
}

void OnlineService::SendLocked(const std::string& text) {
  if (channel_) channel_->Send(text);
}

void OnlineService::Send(Purpose purpose, const std::function<std::string(int)>& encode, const std::string& player) {
  if (!channel_ || step_ == Step::kIdle || step_ == Step::kOpening) return;
  const int seq = next_seq_++;
  pending_[seq] = {purpose, player, search_epoch_, now_};
  channel_->Send(encode(seq));
}

void OnlineService::TestConnection() {
  Locked lock(*this);
  if (!settings_.enabled || testing_) return;
  test_.reset();
  testing_ = true;
  StartTest();
}

void OnlineService::StartTest() {
  test_needs_identity_ = !EnsureIdentity();
  if (test_needs_identity_) return;
  auto state = std::make_shared<TestState>();
  test_probe_ = state;
  ConnectionTestRequest request;
  request.url = config_.url;
  request.identity = identity_;
  request.name = SessionName();
  request.guest = config_.guest;
  request.build = config_.build;
  request.stun = config_.stun;
  request.upnp = config_.upnp;
  request.game_ports = config_.game_ports;
  if (config_.game_socket && config_.game_socket->is_open()) request.game_port = config_.game_socket->bound_port();
  auto work = [state, run = config_.test, request = std::move(request)] {
    std::optional<ConnectionReport> report;
    std::string stopped;
    try {
      report = run(request);
    } catch (const std::exception& e) {
      stopped = e.what();
    } catch (...) {
      stopped = "unknown error";
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->report = std::move(report);
    state->stopped = std::move(stopped);
    state->done = true;
  };
  if (config_.synchronous_probes) work();
  else std::thread(work).detach();
}

void OnlineService::CollectTest() {
  if (!test_probe_) return;
  std::optional<ConnectionReport> finished;
  std::string stopped;
  {
    std::lock_guard<std::mutex> lock(test_probe_->mutex);
    if (!test_probe_->done) return;
    finished = test_probe_->report;
    stopped = test_probe_->stopped;
  }
  test_probe_.reset();
  testing_ = false;
  if (!finished) {
    Log("DEBUG_SAULO online connection test stopped without a report: " + stopped);
    return;
  }
  test_ = finished;
  ++tests_finished_;
  SendTestBeacon(*finished);
  for (const std::string& detail : finished->details) Log("DEBUG_SAULO online connection test " + detail);
  Log("DEBUG_SAULO online connection test " + finished->verdict);
}

void OnlineService::SendTestBeacon(const ConnectionReport& report) {
  if (!config_.test_beacon) return;
  try {
    config_.test_beacon("/telemetry/test?" + ConnectionTestBeaconQuery(report, lobby_diag_, config_.version,
                                                                       config_.platform, config_.platform, "x64"));
  } catch (...) {
  }
}

void OnlineService::Notice(const std::string& text) {
  notices_.push_back(text);
  notice_ = text;
  notice_at_ = now_;
}

void OnlineService::Log(const std::string& line) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  log_queue_.push_back(line);
}

void OnlineService::FlushLog() {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    lines.swap(log_queue_);
  }
  if (!config_.log) return;
  for (const std::string& line : lines) config_.log(line);
}

std::vector<std::string> OnlineService::TakeNotices() {
  Locked lock(*this);
  std::vector<std::string> out;
  out.swap(notices_);
  return out;
}

// False while the answer is not known yet: this runs on the thread that presents, which must never wait for it.
bool OnlineService::EnsureIdentity() {
  if (identity_loaded_) return true;
  if (now_ < next_identity_try_) return false;
  Identity identity = config_.identity ? config_.identity() : Identity{};
  if (identity.pending) {
    next_identity_try_ = now_ + kIdentityRetrySeconds;
    return false;
  }
  identity_ = std::move(identity);
  identity_loaded_ = true;
  if (!identity_.ok()) Log("DEBUG_SAULO online identity unavailable " + identity_.error);
  return true;
}

std::string OnlineService::SessionName() const {
  std::string name = CleanOnlineName(settings_.name);
  if (!identity_.ephemeral) return name;
  return name.substr(0, kMaxOnlineNameChars - 2) + "-2";
}

std::string OnlineService::NameOf(const std::string& player) const {
  for (const Friend& f : friends_) {
    if (f.player == player) return f.name;
  }
  return "Your friend";
}

void OnlineService::SetPlayerMatchQos(const std::vector<uint8_t>& data) {
  Locked lock(*this);
  const std::string hex = HexOf(data);
  if (hex == hosting_qos_) return;
  hosting_qos_ = hex;
  Log("DEBUG_SAULO player_match qos listen bytes=" + std::to_string(data.size()));
  if (hosting_ && Connected()) SendPlayerMatchHost();
}

void OnlineService::SetPlayerMatchSearch(bool on) {
  Locked lock(*this);
  if (on) BeginPlayerMatchSearch();
  else StopPlayerMatchSearch();
  search_from_game_ = false;
}

void OnlineService::BeginPlayerMatchSearch() {
  if (searching_) return;
  searching_ = true;
  ++search_epoch_;
  next_search_ = 0;
  results_.clear();
  search_generation_ = 0;
  search_answered_at_ = -1e9;
  Log("DEBUG_SAULO player_match search begin");
}

void OnlineService::StopPlayerMatchSearch() {
  if (!searching_) return;
  searching_ = false;
  ++search_epoch_;
  next_search_ = 0;
}

bool OnlineService::PlayerMatchSearchInFlight(double* sent_at) const {
  bool found = false;
  for (const auto& [seq, request] : pending_) {
    if (request.purpose != Purpose::kPlayerMatchSearch || request.search_epoch != search_epoch_) continue;
    if (!found || request.sent_at < *sent_at) *sent_at = request.sent_at;
    found = true;
  }
  return found;
}

OnlineService::PlayerMatchSearchWait OnlineService::AskPlayerMatchSearch() {
  Locked lock(*this);
  BeginPlayerMatchSearch();
  search_from_game_ = true;
  search_asked_at_ = now_;
  PlayerMatchSearchWait wait;
  wait.epoch = search_epoch_;
  if (!Connected()) return wait;
  double sent_at = 0;
  const bool fresh = now_ - search_answered_at_ < kSearchFreshSeconds;
  if (!fresh && !PlayerMatchSearchInFlight(&sent_at)) {
    SendPlayerMatchSearch();
    Log("DEBUG_SAULO player_match search requested for guest");
    sent_at = now_;
  }
  const bool nothing_to_show = results_.empty() && !accepted_invite_;
  wait.pending = nothing_to_show && !fresh && now_ - sent_at < kSearchTimeoutSeconds;
  if (wait.pending) Log("DEBUG_SAULO player_match search pending");
  return wait;
}

bool OnlineService::PlayerMatchSearchPending(uint32_t epoch) const {
  std::lock_guard<std::mutex> lock(mutex_);
  double sent_at = 0;
  if (!searching_ || epoch != search_epoch_ || !PlayerMatchSearchInFlight(&sent_at)) return false;
  return now_ - sent_at < kSearchTimeoutSeconds;
}

std::vector<PlayerMatchAd> OnlineService::PlayerMatchResults(uint32_t epoch) const {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!searching_ || epoch != search_epoch_) return {};
  }
  return PlayerMatchResults();
}

void OnlineService::SendPlayerMatchSearch() {
  next_search_ = now_ + kSearchSeconds;
  Send(Purpose::kPlayerMatchSearch, [](int seq) { return EncodePlayerMatchSearch(seq); });
}

bool OnlineService::SignedIn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return settings_.enabled && Connected();
}

ServiceCapabilities OnlineService::Capabilities() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return capabilities_;
}

OnlineProfile OnlineService::Profile() const {
  std::lock_guard<std::mutex> lock(mutex_);
  OnlineProfile profile;
  profile.signed_in = settings_.enabled && Connected();
  profile.friend_code = friend_code_;
  profile.name = greeted_name_;
  return profile;
}

}  // namespace tmnt::online
