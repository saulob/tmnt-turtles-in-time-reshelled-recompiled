#include "online/match_session.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "online/net_addr.h"
#include "online/stun.h"

namespace tmnt::online {
namespace {

constexpr double kPeerEndpointWaitSeconds = 10.0;
constexpr double kRelayStatsSeconds = 10.0;
constexpr double kTransportConfirmSeconds = 5.0;
// As long as the service keeps a lost member's seat.
constexpr double kRelayResumeSeconds = 30.0;

// The relay rides the room connections; the service keeps the pair's token and seats through a lost one.
bool RelayCanReturn(const std::string& reason) {
  return reason == "service_disconnected" || reason == "peer_disconnected" || reason == "peer_unavailable" ||
         reason == "unavailable";
}

uint64_t Milliseconds(double seconds) { return uint64_t(std::max(0.0, seconds) * 1000.0); }

uint64_t MonotonicMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

const char* Flag(bool value) { return value ? "true" : "false"; }

std::vector<netplay::Endpoint> ResolveDefault(const std::string& stun_config) {
  std::vector<netplay::Endpoint> servers;
  if (!net::SocketsReady()) return servers;
  for (const auto& [host, port] : net::StunServers(stun_config)) {
    if (const uint32_t ip = net::ResolveIpv4(host)) servers.push_back({ip, port});
  }
  return servers;
}

}  // namespace

const char* MatchTransportName(MatchTransport transport) {
  switch (transport) {
    case MatchTransport::kDirectProbing: return "direct_probing";
    case MatchTransport::kDirect: return "direct";
    case MatchTransport::kRelay: return "relay";
    case MatchTransport::kFailed: return "failed";
    default: return "pending";
  }
}

struct MatchSession::StunLookup {
  std::mutex mutex;
  bool done = false;
  std::vector<netplay::Endpoint> servers;
};

// Sees the gameplay socket's datagrams before the game: round-trip probes between participants, then STUN and punches.
class MatchSession::Tap final : public netplay::DatagramTap {
 public:
  explicit Tap(MatchSession& session) : session_(session) {}

  bool Take(const netplay::Endpoint& from, const uint8_t* data, size_t size, netplay::Transport& reply) override {
    if (PeerPing::LooksLikePing(data, size)) {
      HandlePing(from, data, size, reply);
      return true;
    }
    return session_.rendezvous_.Take(from, data, size, reply);
  }

 private:
  void HandlePing(const netplay::Endpoint& from, const uint8_t* data, size_t size, netplay::Transport& reply) {
    std::vector<uint8_t> answer;
    {
      std::lock_guard<std::mutex> lock(session_.ping_mutex_);
      int slot = netplay::RelayPeerSlot(from);
      for (int s = 1; !slot && s <= netplay::kMaxSlot; ++s) {
        if (session_.direct_endpoints_[s].port && session_.direct_endpoints_[s] == from) slot = s;
      }
      if (!slot || !session_.ping_.Take(slot, data, size, MonotonicMs(), answer)) return;
    }
    if (!answer.empty()) reply.Send(from, answer.data(), answer.size());
  }

  MatchSession& session_;
};

MatchSession::MatchSession(MatchSessionConfig config) : config_(std::move(config)), rendezvous_([this](const std::string& line) { Log(line); }) {
  if (!config_.resolve_stun) config_.resolve_stun = ResolveDefault;
  tap_ = std::make_unique<Tap>(*this);
  if (config_.socket) {
    config_.socket->CloseRelay();
    config_.socket->ResetTraffic();
    config_.socket->SetTap(tap_.get());
    if (config_.relay_token && config_.send_binary) {
      config_.socket->OpenRelay(config_.relay_token, config_.my_slot, config_.send_binary);
      relay_open_ = true;
    }
  }
  Log(std::string("DEBUG_SAULO match session begin slot=") + std::to_string(config_.my_slot) +
      " relay_available=" + Flag(relay_open_) + " mesh=" + Flag(config_.full_mesh));
  BeginStun();
}

MatchSession::~MatchSession() { Stop(); }

void MatchSession::Log(const std::string& line) const {
  if (config_.log) config_.log(config_.diag.empty() ? line : line + " diag=" + config_.diag);
}

void MatchSession::Emit(const std::string& name, int slot, std::vector<std::pair<std::string, std::string>> fields) const {
  if (config_.event) config_.event(SessionEvent{name, slot, std::move(fields)});
}

bool MatchSession::Required(int slot) const {
  if (slot < 1 || slot > netplay::kMaxSlot || slot == config_.my_slot) return false;
  return config_.full_mesh || config_.my_slot == 1 || slot == 1;
}

MatchSession::Link& MatchSession::Ensure(int slot) {
  std::optional<Link>& entry = links_[slot];
  if (!entry) {
    entry.emplace();
    entry->slot = slot;
  }
  return *entry;
}

MatchSession::Link* MatchSession::Find(int slot) {
  return slot >= 1 && slot <= netplay::kMaxSlot && links_[slot] ? &*links_[slot] : nullptr;
}

const MatchSession::Link* MatchSession::Find(int slot) const {
  return slot >= 1 && slot <= netplay::kMaxSlot && links_[slot] ? &*links_[slot] : nullptr;
}

bool MatchSession::Coordinated(const Link& link) const {
  return config_.relay_token && relay_open_ && !link.relay_dead && config_.socket;
}

int MatchSession::ElapsedMs(const Link& link) const {
  return link.probe_started < 0 ? 0 : int((now_ - link.probe_started) * 1000.0);
}

void MatchSession::BeginStun() {
  if (!config_.socket) return;
  auto lookup = std::make_shared<StunLookup>();
  stun_lookup_ = lookup;
  auto work = [lookup, resolve = config_.resolve_stun, stun = config_.stun] {
    std::vector<netplay::Endpoint> servers = resolve(stun);
    std::lock_guard<std::mutex> lock(lookup->mutex);
    lookup->servers = std::move(servers);
    lookup->done = true;
  };
  if (config_.synchronous_probes) work();
  else std::thread(work).detach();
}

std::optional<std::vector<netplay::Endpoint>> MatchSession::StunServers() {
  if (stun_servers_) return stun_servers_;
  if (!stun_lookup_) return std::vector<netplay::Endpoint>();
  std::vector<netplay::Endpoint> servers;
  {
    std::lock_guard<std::mutex> lock(stun_lookup_->mutex);
    if (!stun_lookup_->done) return std::nullopt;
    servers = stun_lookup_->servers;
  }
  stun_lookup_.reset();
  stun_servers_ = servers;
  return servers;
}

void MatchSession::OnMembers(const std::vector<RoomMember>& members, double now) {
  now_ = now;
  for (const RoomMember& member : members) {
    if (!Required(member.slot)) continue;
    Link& link = Ensure(member.slot);
    link.name = member.name;
    link.member = member.state == "connected" ? MemberState::kConnected
                  : member.state == "connecting" ? MemberState::kConnecting
                  : member.state == "disconnected" ? MemberState::kDisconnected : MemberState::kJoined;
    if (link.relay_resume_deadline > 0 && link.member != MemberState::kDisconnected) {
      link.relay_resume_deadline = 0;
      Log("DEBUG_SAULO relay resumed peer=" + std::to_string(link.slot));
    }
    if (link.setup == Setup::kNone && link.establishment.phase() == Establishment::Phase::kIdle &&
        (link.member == MemberState::kConnecting || link.member == MemberState::kConnected)) {
      BeginLink(link, now);
    }
  }
  for (int slot = 1; slot <= netplay::kMaxSlot; ++slot) {
    if (!links_[slot]) continue;
    const bool present = std::any_of(members.begin(), members.end(), [slot](const RoomMember& m) { return m.slot == slot; });
    if (!present) OnPeerLeft(slot, "left", now);
  }
}

void MatchSession::BeginLink(Link& link, double now) {
  link.transport = MatchTransport::kDirectProbing;
  link.establishment.Begin(now);
  link.setup = Setup::kNone;
  Log("DEBUG_SAULO network establishment begin peer=" + std::to_string(link.slot) +
      " deadline_ms=" + std::to_string(int(kEstablishmentBudgetSeconds * 1000)));
  if (!config_.socket || !config_.socket->is_open()) {
    FailLink(link, "socket_unbound", {});
    return;
  }
  if (stun_done_) StartDirectProbe(link, now);
}

void MatchSession::StartDirectProbe(Link& link, double now) {
  link.setup = Setup::kWaitingPeer;
  link.peer_deadline = now + kPeerEndpointWaitSeconds;
  link.probe_started = now;
  link.direct_deadline = now + kDirectProbeWindowSeconds;
  Log("DEBUG_SAULO direct probe begin peer=" + std::to_string(link.slot) +
      " budget_ms=" + std::to_string(int(kDirectProbeWindowSeconds * 1000)) + " relay_available=" + Flag(Coordinated(link)));
  Emit("direct_attempt", link.slot);
  if (Coordinated(link) && Chooser(link) && !config_.service_forwards_public) {
    RequestRelay(link, "service_public_endpoints_unsupported");
  }
  EvaluatePeer(link);
}

void MatchSession::EvaluatePeer(Link& link) {
  if (!link.info) return;
  if (FailIfServiceCannotForwardPublic(link)) return;
  ConsiderPeer(link);
}

void MatchSession::OnPeer(const PeerInfo& peer, double now) {
  now_ = now;
  if (!Required(peer.slot)) return;
  Link& link = Ensure(peer.slot);
  const bool unchanged = link.info && link.info->public_ip == peer.public_ip && link.info->public_port == peer.public_port &&
                         link.info->ip == peer.ip && link.info->port == peer.port && link.info->lan == peer.lan;
  link.info = peer;
  if (link.name.empty()) link.name = peer.name;
  if (link.establishment.phase() == Establishment::Phase::kIdle && link.setup == Setup::kNone) BeginLink(link, now);
  if (unchanged) return;
  Log("DEBUG_SAULO peer endpoint peer=" + std::to_string(peer.slot) + " same_network=" + Flag(peer.same_network) +
      " public=" + (peer.public_port ? peer.public_ip + ":" + std::to_string(peer.public_port) : std::string("none")) +
      " service_forwards_public=" + Flag(peer.service_forwards_public));
  if (link.setup == Setup::kWaitingPeer || link.setup == Setup::kPunching) EvaluatePeer(link);
}

// Over the Internet only the peer's own STUN endpoint is worth a punch: its observed address with its local port is a
// guess that the peer's endpoint, still on its way, would only replace.
bool MatchSession::PeerEndpointReady(const PeerInfo& peer) const {
  uint32_t address = 0;
  const bool has_public = peer.public_port && netplay::UsableAddress(netplay::ParsePeerAddress(peer.public_ip, address));
  return has_public || peer.same_network;
}

// A service that does not forward public endpoints can never deliver the one this pair is waiting for: with a relay the
// chooser goes straight to it, without one the pair is given up.
bool MatchSession::FailIfServiceCannotForwardPublic(Link& link) {
  const bool setting_up = link.setup == Setup::kWaitingPeer;
  if (!link.info || !setting_up || link.info->service_forwards_public || link.info->same_network ||
      PeerEndpointReady(*link.info)) {
    return false;
  }
  if (Coordinated(link)) {
    if (link.relay_requested) return true;
    Log("DEBUG_SAULO direct unavailable peer=" + std::to_string(link.slot) + " reason=service_public_endpoints_unsupported");
    if (Chooser(link)) RequestRelay(link, "service_public_endpoints_unsupported");
    return true;
  }
  FailLink(link, "service_public_endpoints_unsupported", "Could not connect to " + link.name + ": the online service is out of date.");
  return true;
}

void MatchSession::ConsiderPeer(Link& link) {
  if (!link.info || !PeerEndpointReady(*link.info) || link.relay_requested || link.setup == Setup::kSelected ||
      link.setup == Setup::kFailed || !stun_done_) {
    return;
  }
  if (link.setup == Setup::kWaitingPeer) {
    Log("DEBUG_SAULO peer endpoint ready peer=" + std::to_string(link.slot) + " public=" +
        (link.info->public_port ? link.info->public_ip + ":" + std::to_string(link.info->public_port) : std::string("none")) +
        " same_network=" + Flag(link.info->same_network));
  }
  StartPunch(link);
}

void MatchSession::StartPunch(Link& link) {
  const std::vector<Candidate> candidates = PeerCandidates(*link.info, config_.lan, mapped_);
  if (candidates.empty()) {
    FailLink(link, "no_candidates", {});
    return;
  }
  if (Coordinated(link) && link.setup != Setup::kPunching) {
    link.direct_deadline = std::max(link.direct_deadline, now_ + kMinPunchSeconds);
  }
  link.setup = Setup::kPunching;
  Log("DEBUG_SAULO udp rendezvous begin peer=" + std::to_string(link.slot) + " candidates=" + std::to_string(candidates.size()));
  const uint64_t key = PairPunchKey(config_.match, config_.my_slot, link.slot);
  const uint8_t side = Chooser(link) ? 1 : 2;
  const uint64_t now_ms = Milliseconds(now_);
  config_.socket->Use([&](netplay::Transport& out) { rendezvous_.pair(link.slot).StartPunch(key, side, candidates, now_ms, out); });
}

void MatchSession::Update(double now) {
  now_ = now;
  if (!config_.socket) return;
  bool wanted = false;
  for (const std::optional<Link>& link : links_) wanted = wanted || (link && link->setup != Setup::kFailed);
  if (!wanted) return;

  // A pending STUN lookup must not hold a link past its establishment budget.
  for (std::optional<Link>& entry : links_) {
    if (!entry || entry->setup == Setup::kSelected || entry->setup == Setup::kFailed) continue;
    if (!entry->establishment.ConnectingExpired(now)) continue;
    Log("DEBUG_SAULO network establishment timeout peer=" + std::to_string(entry->slot) +
        " elapsed_ms=" + std::to_string(int(entry->establishment.Elapsed(now) * 1000)) +
        " transport_state=" + MatchTransportName(entry->transport) + " relay_requested=" + Flag(entry->relay_requested));
    FailLink(*entry, "establishment_timeout", "Could not connect to " + entry->name + ".");
  }
  wanted = false;
  for (const std::optional<Link>& link : links_) wanted = wanted || (link && link->setup != Setup::kFailed);
  if (!wanted) return;

  if (!stun_started_ && !stun_done_) {
    const auto servers = StunServers();
    if (!servers) return;
    const uint64_t now_ms = Milliseconds(now);
    const uint16_t local_port = config_.socket->bound_port();
    config_.socket->Use([&](netplay::Transport& out) { rendezvous_.stun().StartStun(*servers, local_port, now_ms, out); });
    stun_started_ = true;
  }

  struct PunchView {
    Rendezvous::Stage stage = Rendezvous::Stage::kIdle;
    Candidate selected;
    std::vector<netplay::Endpoint> heard;
  };
  std::array<PunchView, netplay::kMaxSlot + 1> punches;
  Rendezvous::Stage stun = Rendezvous::Stage::kIdle;
  netplay::Endpoint mapped;
  config_.socket->Pump();
  config_.socket->Use([&](netplay::Transport& out) {
    rendezvous_.Tick(Milliseconds(now), out);
    stun = rendezvous_.stun().stun_stage();
    mapped = rendezvous_.stun().mapped();
    for (int slot = 1; slot <= netplay::kMaxSlot; ++slot) {
      if (const Rendezvous* pair = rendezvous_.find_pair(slot)) {
        punches[slot] = {pair->punch_stage(), pair->selected(), pair->heard()};
      }
    }
  });

  if (stun_started_ && !stun_done_ && (stun == Rendezvous::Stage::kDone || stun == Rendezvous::Stage::kFailed)) {
    stun_done_ = true;
    stun_failed_ = stun == Rendezvous::Stage::kFailed;
    mapped_ = stun_failed_ ? netplay::Endpoint{} : mapped;
    Log(std::string("DEBUG_SAULO stun result ok=") + Flag(!stun_failed_) + " mapped=" +
        (stun_failed_ ? std::string("none") : EndpointText(mapped_)));
    SendEndpoint();
    for (std::optional<Link>& link : links_) {
      if (!link || link->setup != Setup::kNone || link->establishment.phase() != Establishment::Phase::kConnecting) continue;
      StartDirectProbe(*link, now);
    }
  }
  if (!stun_done_) return;

  for (std::optional<Link>& entry : links_) {
    if (!entry) continue;
    Link& link = *entry;
    if (link.setup == Setup::kSelected) {
      PumpRelay(link, now);
      PumpPing(link, now);
      continue;
    }
    if (link.setup == Setup::kNone || link.setup == Setup::kFailed) continue;
    if (link.establishment.ConnectingExpired(now)) {
      Log("DEBUG_SAULO network establishment timeout peer=" + std::to_string(link.slot) +
          " elapsed_ms=" + std::to_string(int(link.establishment.Elapsed(now) * 1000)) +
          " transport_state=" + MatchTransportName(link.transport) + " relay_requested=" + Flag(link.relay_requested));
      FailLink(link, "establishment_timeout", "Could not connect to " + link.name + ".");
      continue;
    }
    const PunchView& view = punches[link.slot];
    if (Coordinated(link)) {
      PumpCoordinated(link, now, view.stage, view.selected);
      continue;
    }
    if (link.setup == Setup::kWaitingPeer) {
      if (now < link.peer_deadline) continue;
      Log("DEBUG_SAULO udp rendezvous failed peer=" + std::to_string(link.slot) + " reason=peer_endpoint_timeout");
      FailLink(link, "no_direct_path", "Could not connect to " + link.name + ": their connection details never arrived.");
      continue;
    }
    if (link.setup != Setup::kPunching) continue;
    if (view.stage == Rendezvous::Stage::kFailed) {
      FailLink(link, "no_direct_path", {});
    } else if (view.stage == Rendezvous::Stage::kDone) {
      SelectDirect(link, view.selected);
    }
  }
}

// The chooser decides: the direct path once its punch is answered, the relay once the window has passed without one.
// The other end only follows that choice, so both ends always use the same transport.
void MatchSession::PumpCoordinated(Link& link, double now, Rendezvous::Stage punch, const Candidate& selected) {
  if (!Chooser(link)) return;
  if (link.relay_requested) {
    if (now - link.probe_started > kDirectProbeWindowSeconds + kTransportConfirmSeconds + kPeerEndpointWaitSeconds) {
      FailLink(link, "transport_unconfirmed", "Could not connect to " + link.name + ".");
    }
    return;
  }
  if (link.setup == Setup::kPunching && punch == Rendezvous::Stage::kDone) {
    SelectDirect(link, selected);
    return;
  }
  const bool punch_failed = link.setup == Setup::kPunching && punch == Rendezvous::Stage::kFailed;
  if (now < link.direct_deadline && !punch_failed) return;
  std::string why = "direct_timeout";
  if (link.setup == Setup::kWaitingPeer) {
    why = stun_failed_ && !(link.info && link.info->same_network) ? "stun_failed"
          : link.info                                               ? "no_public_endpoint"
                                                                    : "peer_endpoint_missing";
  } else if (punch_failed) {
    why = "hole_punch_failed";
  }
  RequestRelay(link, why);
}

void MatchSession::RequestRelay(Link& link, const std::string& why) {
  if (link.relay_requested || link.setup == Setup::kSelected || link.setup == Setup::kFailed) return;
  link.relay_requested = true;
  link.direct_elapsed_ms = ElapsedMs(link);
  link.fallback_reason = why;
  link.direct_result = "failed:" + why;
  Log("DEBUG_SAULO direct path not established peer=" + std::to_string(link.slot) +
      " elapsed_ms=" + std::to_string(link.direct_elapsed_ms) + " reason=" + why);
  Log("DEBUG_SAULO relay fallback begin peer=" + std::to_string(link.slot));
  Emit("direct_result", link.slot, {{"result", "failed"}, {"reason", why}, {"ms", std::to_string(link.direct_elapsed_ms)}});
  Emit("relay_fallback", link.slot, {{"reason", why}});
  link.relay_request_seq = ++endpoint_seq_;
  if (config_.send_text) config_.send_text(EncodeMatchTransport(link.relay_request_seq, link.slot, "relay"));
}

// The other end of a pair waits for this choice whenever the session has a relay, even if this end's relay just died.
void MatchSession::AnnounceDirect(const Link& link) {
  if (config_.relay_token && config_.send_text) config_.send_text(EncodeMatchTransport(++endpoint_seq_, link.slot, "direct"));
}

void MatchSession::SelectDirect(Link& link, const Candidate& selected) {
  const bool tell = Chooser(link);
  link.setup = Setup::kSelected;
  link.transport = MatchTransport::kDirect;
  link.direct_elapsed_ms = ElapsedMs(link);
  link.direct_result = "ok";
  link.endpoint = selected.endpoint;
  config_.socket->CloseRelayPeer(link.slot);
  config_.socket->SetPeerEndpoint(link.slot, selected.endpoint);
  {
    std::lock_guard<std::mutex> lock(ping_mutex_);
    direct_endpoints_[link.slot] = selected.endpoint;
    ping_.Reset(link.slot);
  }
  Log("DEBUG_SAULO transport selected peer=" + std::to_string(link.slot) + " selected=direct relay_available=" +
      Flag(Coordinated(link)) + " relay_used=false endpoint=" + EndpointText(selected.endpoint) +
      " type=" + CandidateTypeName(selected.type) + " elapsed_ms=" + std::to_string(link.direct_elapsed_ms));
  Emit("direct_result", link.slot, {{"result", "ok"}, {"ms", std::to_string(link.direct_elapsed_ms)}});
  Emit("transport_selected", link.slot, {{"transport", "direct"}, {"ms", std::to_string(link.direct_elapsed_ms)}});
  if (tell) AnnounceDirect(link);
  Assign(link, selected.endpoint, false);
}

void MatchSession::SelectRelay(Link& link) {
  link.setup = Setup::kSelected;
  link.transport = MatchTransport::kRelay;
  link.relay_used = true;
  link.relay_result = "ok";
  if (link.direct_elapsed_ms < 0) link.direct_elapsed_ms = ElapsedMs(link);
  config_.socket->Use([&](netplay::Transport&) { rendezvous_.StopPair(link.slot); });
  config_.socket->SelectRelay(link.slot);
  {
    std::lock_guard<std::mutex> lock(ping_mutex_);
    ping_.Reset(link.slot);
  }
  link.next_relay_stats = now_ + kRelayStatsSeconds;
  Log("DEBUG_SAULO transport selected peer=" + std::to_string(link.slot) + " selected=relay relay_available=true relay_used=true" +
      " fallback_reason=" + (link.fallback_reason.empty() ? std::string("host_chose_relay") : link.fallback_reason) +
      " elapsed_ms=" + std::to_string(int(link.establishment.Elapsed(now_) * 1000)));
  Emit("relay_result", link.slot, {{"result", "ok"}});
  Emit("transport_selected", link.slot, {{"transport", "relay"}, {"ms", std::to_string(int(link.establishment.Elapsed(now_) * 1000))}});
  Assign(link, netplay::RelayPeerEndpoint(link.slot), true);
}

void MatchSession::Assign(Link& link, const netplay::Endpoint& peer, bool relay) {
  NetplayAssignment assignment;
  assignment.slot = link.slot;
  assignment.peer_ip = net::IpText(peer.address);
  assignment.peer_port = peer.port;
  assignment.generation = config_.generation;
  assignment.relay = relay;
  link.assignment = assignment;
  if (link.establishment.Select(now_)) {
    Log("DEBUG_SAULO network establishment ready peer=" + std::to_string(link.slot) +
        " transport=" + (relay ? "relay" : "direct") + " elapsed_ms=" + std::to_string(int(link.establishment.Elapsed(now_) * 1000)));
  }
  if (config_.notice) config_.notice("Connected with " + link.name + ".");
}

void MatchSession::OnTransport(int a, int b, const std::string& transport, int by, double now) {
  now_ = now;
  const int me = config_.my_slot;
  if (a != me && b != me) {
    Log("DEBUG_SAULO transport confirmed pair=" + std::to_string(a) + "-" + std::to_string(b) + " transport=" + transport +
        " by=" + std::to_string(by));
    return;
  }
  Link* link = Find(a == me ? b : a);
  if (!link || link->setup == Setup::kFailed || link->setup == Setup::kSelected) {
    Log("DEBUG_SAULO transport choice ignored transport=" + transport);
    return;
  }
  if (Chooser(*link)) {
    if (transport == "relay" && link->relay_requested) SelectRelay(*link);
    return;
  }
  if (!Coordinated(*link) || link->setup == Setup::kNone) {
    Log("DEBUG_SAULO transport choice ignored transport=" + transport);
    return;
  }
  Log("DEBUG_SAULO transport choice from peer=" + std::to_string(link->slot) + " transport=" + transport);
  if (transport == "relay") {
    link->fallback_reason = "chooser_chose_relay";
    link->direct_result = "failed:chooser_chose_relay";
    link->direct_elapsed_ms = ElapsedMs(*link);
    Emit("direct_result", link->slot, {{"result", "failed"}, {"reason", "chooser_chose_relay"}});
    Emit("relay_fallback", link->slot, {{"reason", "chooser_chose_relay"}});
    SelectRelay(*link);
    return;
  }
  // The chooser heard this side's answer, so this side heard it: its own proven candidate, else where its punches came from.
  Candidate chosen;
  bool known = false;
  config_.socket->Use([&](netplay::Transport&) {
    if (const Rendezvous* pair = rendezvous_.find_pair(link->slot)) {
      if (pair->punch_stage() == Rendezvous::Stage::kDone) {
        chosen = pair->selected();
        known = true;
      } else if (!pair->heard().empty()) {
        chosen = {CandidateType::kPeer, pair->heard().front()};
        known = true;
      }
    }
  });
  if (!known) {
    FailLink(*link, "direct_handshake_failed", {});
    return;
  }
  SelectDirect(*link, chosen);
}

void MatchSession::OnRelayClosed(int peer_slot, const std::string& reason, double now) {
  now_ = now;
  Log("DEBUG_SAULO relay closed peer=" + std::to_string(peer_slot) + " reason=" + reason);
  for (int slot = 1; slot <= netplay::kMaxSlot; ++slot) {
    Link* link = Find(slot);
    if (!link || (peer_slot && peer_slot != slot)) continue;
    if (link->transport == MatchTransport::kRelay) {
      if (!RelayCanReturn(reason)) {
        FailLink(*link, "relay_closed", "The connection to " + link->name + " was lost.");
      } else if (link->relay_resume_deadline == 0) {
        link->relay_resume_deadline = now + kRelayResumeSeconds;
        Log("DEBUG_SAULO relay suspended peer=" + std::to_string(slot) + " reason=" + reason);
      }
    } else if (link->transport == MatchTransport::kDirectProbing) {
      if (reason == "peer_disconnected" || link->relay_requested) {
        FailLink(*link, reason == "peer_disconnected" ? "peer_disconnected" : "relay_unavailable", {});
      } else {
        // Without its relay the pair can still connect directly, by the rules of a pair that never had one.
        if (config_.socket) config_.socket->CloseRelayPeer(slot);
        link->relay_dead = true;
      }
    }
  }
}

void MatchSession::OnPeerLost(int slot) {
  if (Link* link = Find(slot)) link->member = MemberState::kDisconnected;
}

void MatchSession::OnPeerLeft(int slot, const std::string& reason, double now) {
  now_ = now;
  Link* link = Find(slot);
  if (!link) return;
  Log("DEBUG_SAULO peer left peer=" + std::to_string(slot) + " reason=" + reason);
  LogSummary(*link);
  Emit("peer_disconnect", slot, {{"reason", reason}});
  if (config_.socket) {
    config_.socket->CloseRelayPeer(slot);
    config_.socket->Use([&](netplay::Transport&) { rendezvous_.StopPair(slot); });
  }
  {
    std::lock_guard<std::mutex> lock(ping_mutex_);
    ping_.Reset(slot);
    direct_endpoints_[slot] = {};
  }
  if (slot == 1 && config_.my_slot != 1) fatal_ = true;
  links_[slot].reset();
}

void MatchSession::FailLink(Link& link, const std::string& reason, const std::string& notice) {
  if (link.setup == Setup::kFailed) return;
  const bool established = link.transport == MatchTransport::kDirect || link.transport == MatchTransport::kRelay;
  Log(std::string(established ? "DEBUG_SAULO transport lost" : "DEBUG_SAULO udp rendezvous failed") + " peer=" +
      std::to_string(link.slot) + " reason=" + reason);
  if (!established) link.transport = MatchTransport::kFailed;
  link.setup = Setup::kFailed;
  link.failure = reason;
  if (link.relay_used || link.relay_requested) {
    link.relay_result = "failed:" + reason;
    Emit("relay_result", link.slot, {{"result", "failed"}, {"reason", reason}});
  } else if (!established && link.direct_result == "none") {
    link.direct_result = "failed:" + reason;
    Emit("direct_result", link.slot, {{"result", "failed"}, {"reason", reason}});
  }
  link.establishment.Fail(now_, reason);
  ReportLink(link);
  Emit("peer_disconnect", link.slot, {{"reason", reason}});
  if (config_.socket) {
    config_.socket->CloseRelayPeer(link.slot);
    config_.socket->Use([&](netplay::Transport&) { rendezvous_.StopPair(link.slot); });
  }
  LogSummary(link);
  if (link.slot == 1 && config_.my_slot != 1) fatal_ = true;
  if (config_.notice) {
    config_.notice(notice.empty() ? "Could not connect to " + link.name + ": no direct connection between your networks." : notice);
  }
}

void MatchSession::LogSummary(Link& link) {
  if (link.summary_logged) return;
  link.summary_logged = true;
  const netplay::PeerTraffic traffic = config_.socket ? config_.socket->peer_traffic(link.slot) : netplay::PeerTraffic{};
  const char* selected = link.transport == MatchTransport::kDirect ? "direct"
                         : link.transport == MatchTransport::kRelay ? "relay"
                         : link.transport == MatchTransport::kFailed ? "failed" : "none";
  Log(std::string("DEBUG_SAULO transport summary peer=") + std::to_string(link.slot) + " relay_available=" +
      Flag(link.relay_used || link.relay_requested || Coordinated(link)) + " selected=" + selected +
      " relay_used=" + Flag(link.relay_used) +
      (link.fallback_reason.empty() ? std::string() : " fallback_reason=" + link.fallback_reason) +
      (link.failure.empty() ? std::string() : " failure=" + link.failure) +
      " direct_elapsed_ms=" + std::to_string(link.direct_elapsed_ms >= 0 ? link.direct_elapsed_ms : ElapsedMs(link)) +
      " tx_packets=" + std::to_string(link.relay_used ? traffic.relay_tx_packets : traffic.udp_tx_packets) +
      " rx_packets=" + std::to_string(link.relay_used ? traffic.relay_rx_packets : traffic.udp_rx_packets));
}

void MatchSession::PumpRelay(Link& link, double now) {
  if (link.transport != MatchTransport::kRelay || !config_.socket) return;
  if (link.relay_resume_deadline > 0 && now >= link.relay_resume_deadline) {
    FailLink(link, "relay_closed", "The connection to " + link.name + " was lost.");
    return;
  }
  if (now < link.next_relay_stats) return;
  link.next_relay_stats = now + kRelayStatsSeconds;
  const netplay::PeerTraffic traffic = config_.socket->peer_traffic(link.slot);
  Log("DEBUG_SAULO relay stats peer=" + std::to_string(link.slot) + " tx_packets=" + std::to_string(traffic.relay_tx_packets) +
      " rx_packets=" + std::to_string(traffic.relay_rx_packets) + " rejected=" + std::to_string(traffic.relay_rejected));
}

void MatchSession::PumpPing(Link& link, double) {
  const netplay::Endpoint to = link.transport == MatchTransport::kRelay ? netplay::RelayPeerEndpoint(link.slot)
                               : link.endpoint                         ? *link.endpoint
                                                                       : netplay::Endpoint{};
  if (!to.port) return;
  std::optional<std::vector<uint8_t>> probe;
  {
    std::lock_guard<std::mutex> lock(ping_mutex_);
    probe = ping_.Due(link.slot, MonotonicMs());
  }
  if (probe) config_.socket->Send(to, probe->data(), probe->size());
}

void MatchSession::SendEndpoint() {
  if (endpoint_sent_ || !config_.socket || !config_.send_text) return;
  endpoint_sent_ = true;
  // The service refuses a mapping that is not a public address, which would leave the endpoint out altogether.
  const bool mapped = mapped_.port && net::Classify(mapped_.address) == net::AddressKind::kPublic;
  config_.send_text(EncodeMatchEndpoint(++endpoint_seq_, config_.socket->bound_port(), config_.lan,
                                        mapped ? net::IpText(mapped_.address) : std::string(), mapped ? mapped_.port : 0));
}

void MatchSession::ResendEndpoint() {
  if (!stun_done_) return;
  endpoint_sent_ = false;
  SendEndpoint();
}

void MatchSession::ResendReports() {
  for (const std::optional<Link>& entry : links_) {
    if (!entry) continue;
    if (entry->link != NetplayLink::kNone || entry->setup == Setup::kFailed) ReportLink(*entry);
    if (entry->transport == MatchTransport::kDirect && Chooser(*entry)) AnnounceDirect(*entry);
  }
}

void MatchSession::OnRefused(int seq, const std::string& code, double now) {
  now_ = now;
  for (std::optional<Link>& entry : links_) {
    if (!entry || !entry->relay_requested || entry->relay_request_seq != seq || code == "already_set") continue;
    Log("DEBUG_SAULO relay refused peer=" + std::to_string(entry->slot) + " code=" + code);
    FailLink(*entry, "relay_unavailable", {});
  }
}

void MatchSession::OnBinary(const std::string& frame) {
  if (config_.socket) config_.socket->DeliverRelay(reinterpret_cast<const uint8_t*>(frame.data()), frame.size());
}

void MatchSession::ReportLink(const Link& link) {
  if (!config_.send_text) return;
  const bool up = link.link == NetplayLink::kConnected || link.link == NetplayLink::kFighting;
  const char* state = link.setup == Setup::kFailed ? "lost" : up ? "connected" : "connecting";
  config_.send_text(EncodeMatchLink(++endpoint_seq_, link.slot, state));
}

void MatchSession::SetPeerLink(int slot, NetplayLink link) {
  Link* entry = Find(slot);
  if (!entry || entry->link == link) return;
  entry->link = link;
  ReportLink(*entry);
}

void MatchSession::Stop() {
  if (!config_.socket) return;
  for (std::optional<Link>& link : links_) {
    if (link) LogSummary(*link);
  }
  config_.socket->SetTap(nullptr);
  config_.socket->Use([&](netplay::Transport&) { rendezvous_.Stop(); });
  config_.socket->CloseRelay();
  config_.socket = nullptr;
  relay_open_ = false;
}

std::vector<PeerView> MatchSession::peers() const {
  std::vector<PeerView> out;
  for (const std::optional<Link>& entry : links_) {
    if (!entry) continue;
    const Link& link = *entry;
    PeerView view;
    view.slot = link.slot;
    view.name = link.name;
    view.member = link.member;
    view.transport = link.transport;
    view.relay_available = link.relay_used || link.relay_requested || Coordinated(link);
    view.relay_used = link.relay_used;
    view.chooser = Chooser(link);
    view.direct_result = link.direct_result;
    view.relay_result = link.relay_result;
    view.fallback_reason = link.fallback_reason;
    view.failure = link.failure;
    view.establishment_ms = link.establishment.phase() == Establishment::Phase::kSelected
                                ? int(link.establishment.Elapsed(now_) * 1000) : -1;
    view.link = link.link;
    {
      std::lock_guard<std::mutex> lock(ping_mutex_);
      view.rtt_ms = ping_.rtt_ms(link.slot);
      view.ping = ping_.stats(link.slot);
    }
    if (config_.socket) view.traffic = config_.socket->peer_traffic(link.slot);
    out.push_back(std::move(view));
  }
  return out;
}

std::optional<PeerView> MatchSession::peer(int slot) const {
  for (PeerView& view : peers()) {
    if (view.slot == slot) return std::move(view);
  }
  return std::nullopt;
}

std::optional<NetplayAssignment> MatchSession::Assignment(int slot) const {
  const Link* link = Find(slot);
  return link ? link->assignment : std::nullopt;
}

std::vector<NetplayAssignment> MatchSession::Assignments() const {
  std::vector<NetplayAssignment> out;
  for (const std::optional<Link>& link : links_) {
    if (link && link->assignment) out.push_back(*link->assignment);
  }
  return out;
}

}  // namespace tmnt::online
