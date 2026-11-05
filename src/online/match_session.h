#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "netplay/online_game_socket.h"
#include "online/establishment.h"
#include "online/match_types.h"
#include "online/peer_ping.h"
#include "online/protocol.h"
#include "online/rendezvous.h"

namespace tmnt::online {

// From this side's endpoint advertisement (and again from its first punch): how long a direct path may take before a
// pair that has the service's relay falls back to it.
inline constexpr double kDirectProbeWindowSeconds = 2.5;
// A direct path that starts punching late still gets this long before the chooser falls back to the relay.
inline constexpr double kMinPunchSeconds = 1.5;

// One operational event, for the local log's counterpart in telemetry.
struct SessionEvent {
  std::string name;
  int slot = 0;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct MatchSessionConfig {
  std::string match;
  std::string diag;
  int my_slot = 0;
  uint32_t relay_token = 0;
  uint32_t generation = 0;
  netplay::OnlineGameSocket* socket = nullptr;
  std::vector<std::string> lan;
  std::string stun;
  std::function<std::vector<netplay::Endpoint>(const std::string& stun)> resolve_stun;
  bool synchronous_probes = false;
  bool service_forwards_public = true;
  // Every pair, or only the pairs with the host (slot 1).
  bool full_mesh = false;
  std::function<void(const std::string&)> log;
  std::function<void(std::string&&)> send_text;
  std::function<bool(std::vector<uint8_t>&&)> send_binary;
  std::function<void(const std::string&)> notice;
  std::function<void(const SessionEvent&)> event;
};

// What one remote participant looks like to the user, the game and support.
struct PeerView {
  int slot = 0;
  std::string name;
  MemberState member = MemberState::kJoined;
  MatchTransport transport = MatchTransport::kPending;
  bool relay_available = false;
  bool relay_used = false;
  bool chooser = false;  // this side chooses the transport of the pair
  std::string direct_result;  // none, ok, or failed:<reason>
  std::string relay_result;   // none, ok, or failed:<reason>
  std::string fallback_reason;
  std::string failure;
  int establishment_ms = -1;
  NetplayLink link = NetplayLink::kNone;
  std::optional<int> rtt_ms;
  PingStats ping;
  netplay::PeerTraffic traffic;
};

// One match's network establishment for this participant: a single STUN answer for the gameplay socket, then, for every
// remote participant this side must reach, Direct first and the service's relay second. Pairs are independent: one on
// the relay moves no other. Methods run on the service's thread, under its lock; the datagram tap runs on the socket's.
class MatchSession {
 public:
  explicit MatchSession(MatchSessionConfig config);
  ~MatchSession();
  MatchSession(const MatchSession&) = delete;
  MatchSession& operator=(const MatchSession&) = delete;

  // The room's roster, whenever it changes.
  void OnMembers(const std::vector<RoomMember>& members, double now);
  void OnPeer(const PeerInfo& peer, double now);
  void OnTransport(int a, int b, const std::string& transport, int by, double now);
  void OnRelayClosed(int peer_slot, const std::string& reason, double now);
  void OnPeerLost(int slot);
  void OnPeerLeft(int slot, const std::string& reason, double now);
  void OnBinary(const std::string& frame);
  // The room was lost and found again: the service may have forgotten this side's endpoint.
  void ResendEndpoint();
  // And it forgot this side's link states and may have missed a transport choice.
  void ResendReports();
  // The room refused a request of this session.
  void OnRefused(int seq, const std::string& code, double now);
  void Update(double now);
  // The game's own report about its traffic with a peer.
  void SetPeerLink(int slot, NetplayLink link);
  // Leaves the relay and the rendezvous; the socket is left bound.
  void Stop();

  std::vector<PeerView> peers() const;
  std::optional<PeerView> peer(int slot) const;
  std::optional<NetplayAssignment> Assignment(int slot) const;
  std::vector<NetplayAssignment> Assignments() const;
  // The pair with the host failed on a guest: nothing can be played from this seat.
  bool fatal() const { return fatal_; }
  const std::string& match() const { return config_.match; }
  int my_slot() const { return config_.my_slot; }

 private:
  enum class Setup { kNone, kWaitingPeer, kPunching, kSelected, kFailed };
  struct Link {
    int slot = 0;
    std::string name;
    MemberState member = MemberState::kJoined;
    std::optional<PeerInfo> info;
    Setup setup = Setup::kNone;
    MatchTransport transport = MatchTransport::kPending;
    Establishment establishment;
    double probe_started = -1;
    double direct_deadline = 0;
    double peer_deadline = 0;
    bool relay_requested = false;
    int relay_request_seq = 0;
    bool relay_used = false;
    bool relay_dead = false;
    int direct_elapsed_ms = -1;
    std::string direct_result = "none";
    std::string relay_result = "none";
    std::string fallback_reason;
    std::string failure;
    NetplayLink link = NetplayLink::kNone;
    std::optional<netplay::Endpoint> endpoint;
    std::optional<NetplayAssignment> assignment;
    bool summary_logged = false;
    double relay_resume_deadline = 0;  // set while a relayed pair waits for the service to come back
  };
  class Tap;

  bool Required(int slot) const;
  Link& Ensure(int slot);
  Link* Find(int slot);
  const Link* Find(int slot) const;
  bool Coordinated(const Link& link) const;
  bool Chooser(const Link& link) const { return config_.my_slot < link.slot; }
  void BeginStun();
  void PumpStun(double now);
  std::optional<std::vector<netplay::Endpoint>> StunServers();
  void SendEndpoint();
  void BeginLink(Link& link, double now);
  void StartDirectProbe(Link& link, double now);
  void EvaluatePeer(Link& link);
  void PumpLink(Link& link, double now);
  void PumpCoordinated(Link& link, double now, Rendezvous::Stage punch, const Candidate& selected);
  bool PeerEndpointReady(const PeerInfo& peer) const;
  bool FailIfServiceCannotForwardPublic(Link& link);
  void ConsiderPeer(Link& link);
  void StartPunch(Link& link);
  void RequestRelay(Link& link, const std::string& why);
  void SelectDirect(Link& link, const Candidate& selected);
  void SelectRelay(Link& link);
  void Assign(Link& link, const netplay::Endpoint& peer, bool relay);
  void ReportLink(const Link& link);
  void AnnounceDirect(const Link& link);
  void FailLink(Link& link, const std::string& reason, const std::string& notice);
  void LogSummary(Link& link);
  void PumpRelay(Link& link, double now);
  void PumpPing(Link& link, double now);
  void Emit(const std::string& name, int slot, std::vector<std::pair<std::string, std::string>> fields = {}) const;
  void Log(const std::string& line) const;
  int ElapsedMs(const Link& link) const;

  MatchSessionConfig config_;
  double now_ = 0;
  bool relay_open_ = false;
  bool fatal_ = false;
  std::array<std::optional<Link>, netplay::kMaxSlot + 1> links_;

  struct StunLookup;
  std::shared_ptr<StunLookup> stun_lookup_;
  std::optional<std::vector<netplay::Endpoint>> stun_servers_;
  bool stun_started_ = false;
  bool stun_done_ = false;
  bool stun_failed_ = false;
  bool endpoint_sent_ = false;
  netplay::Endpoint mapped_;
  // Apart from the service's own request numbers, which a room reply is matched against.
  int endpoint_seq_ = 1 << 20;

  RendezvousSet rendezvous_;
  std::unique_ptr<Tap> tap_;
  mutable std::mutex ping_mutex_;
  PeerPing ping_;
  std::array<netplay::Endpoint, netplay::kMaxSlot + 1> direct_endpoints_{};
  uint64_t ping_clock_ms_ = 0;
};

}  // namespace tmnt::online
