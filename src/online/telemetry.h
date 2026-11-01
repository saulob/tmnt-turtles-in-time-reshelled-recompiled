#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "online/peer_ping.h"

namespace tmnt::online {

#define TMNT_REASON_CODES(X)  \
  X(direct_timeout, path)  \
  X(hole_punch_failed, path)  \
  X(stun_failed, path)  \
  X(no_public_endpoint, path)  \
  X(peer_endpoint_missing, path)  \
  X(service_public_endpoints_unsupported, path)  \
  X(chooser_chose_relay, path)  \
  X(socket_unbound, path)  \
  X(no_candidates, path)  \
  X(no_direct_path, path)  \
  X(establishment_timeout, path)  \
  X(transport_unconfirmed, path)  \
  X(direct_handshake_failed, path)  \
  X(relay_unavailable, path)  \
  X(relay_connection_failed, path)  \
  X(relay_closed, path)  \
  X(relay_receive_overflow, path)  \
  X(relay_send_refused, path)  \
  X(peer_disconnected, disconnect)  \
  X(service_disconnected, disconnect)  \
  X(host_left, disconnect)  \
  X(kicked, disconnect)  \
  X(removed, disconnect)  \
  X(expired, disconnect)  \
  X(closed, disconnect)  \
  X(left, disconnect)  \
  X(disconnect, disconnect)  \
  X(disabled, disconnect)  \
  X(host_unreachable, disconnect)  \
  X(timeout, disconnect)  \
  X(link_lost, disconnect)  \
  X(unreachable, disconnect)  \
  X(rate_limit, disconnect)  \
  X(peer_backpressure, disconnect)  \
  X(peer_unavailable, disconnect)  \
  X(unavailable, disconnect)  \
  X(declined, invite)  \
  X(cancelled, invite)  \
  X(accepted_elsewhere, invite)  \
  X(accepted, invite)  \
  X(version_mismatch, service)  \
  X(party_full, service)  \
  X(busy, service)  \
  X(offline, service)  \
  X(not_found, service)  \
  X(not_friends, service)  \
  X(already_invited, service)  \
  X(no_party, service)  \
  X(not_host, service)  \
  X(not_owner, service)  \
  X(already_set, service)  \
  X(limit, service)  \
  X(unauthorized, service)  \
  X(proto, service)  \
  X(bad_game, service)  \
  X(bad_guest, service)  \
  X(bad_request, service)  \
  X(too_large, service)  \
  X(no_session, service)  \
  X(stale, service)  \
  X(internal, service)  \
  X(protocol_mismatch, connection_test)  \
  X(worker_unreachable, connection_test)  \
  X(worker_bad_status, connection_test)  \
  X(worker_unrecognized, connection_test)  \
  X(websocket_failed, connection_test)  \
  X(stun_no_answer, connection_test)  \
  X(signin_refused, connection_test)  \
  X(identity_unavailable, connection_test)  \
  X(direct_unavailable, connection_test)  \
  X(not_usable, connection_test)  \
  X(other, service)

// The only free-form-looking strings telemetry carries. tests/reason-codes.txt lists the same codes and categories.
namespace reason {
#define TMNT_REASON_CONSTANT(code, category) inline constexpr std::string_view code = #code;
TMNT_REASON_CODES(TMNT_REASON_CONSTANT)
#undef TMNT_REASON_CONSTANT
}  // namespace reason

struct ReasonEntry {
  std::string_view code;
  std::string_view category;
};

inline constexpr ReasonEntry kReasons[] = {
#define TMNT_REASON_ENTRY(code, category) {#code, #category},
    TMNT_REASON_CODES(TMNT_REASON_ENTRY)
#undef TMNT_REASON_ENTRY
};

// The code itself when it is a known reason, "other" for anything else.
std::string_view NormalizeReason(std::string_view value);
// "path", "disconnect", "invite", "service" or "connection_test"; empty for an unknown code.
std::string_view ReasonCategory(std::string_view code);

// Keeps [A-Za-z0-9_.:-] up to 48 characters. A value that looks like a friend code, a token (32+ hex), an IP address,
// a URL, a path, an e-mail or free text (whitespace) becomes "redacted" instead of being cleaned up.
std::string SanitizeTelemetryValue(std::string_view value);

struct TelemetryContext {
  std::string game_id, diag, port_version, platform, os, arch, fingerprint;
  int protocol = 0;
  int slot = 0;
  bool host = false;
  int party_size = 1;
};

struct PeerReport {
  int local_slot = 0, remote_slot = 0;
  std::string state;          // connecting|connected|lost|left
  std::string transport;      // pending|direct|relay|failed
  std::string direct_result;  // none|ok|failed (+ reason code)
  std::string relay_result;
  std::string fallback_reason;
  int64_t connected_ms = 0;
  PingStats ping;
  uint64_t sent_packets = 0, received_packets = 0;
  uint32_t reconnects = 0;
  std::string disconnect_reason;
  int64_t establishment_ms = -1;
};

struct SessionReport {
  int64_t duration_ms = 0;
  bool connection_test_usable = false;
  bool worker_reachable = false, stun_ok = false, direct_available = false, relay_available = false;
  std::string compatibility;  // ok|version_mismatch|protocol_mismatch
  std::string disconnect_reason;
};

// Best-effort operational telemetry for support: batched, bounded, privacy-safe. No network code: the integrator
// sends the JSON text of TakeBatch (adding its own seq). Never throws. Not thread-safe.
class Telemetry {
 public:
  static constexpr size_t kMaxQueued = 200;  // oldest events dropped first
  static constexpr size_t kMaxBatchBytes = 8192;
  static constexpr double kMinSecondsBetweenBatches = 10.0;
  static constexpr size_t kMaxFields = 8;
  static constexpr size_t kMaxPeers = 8;

  // A non-personal 12 hex character id for this install/session; never derived from a friend code.
  static std::string NewDiagId();

  // `now` is the zero of the events' t. Without it the first Event/TakeBatch time becomes the zero.
  void SetContext(const TelemetryContext& context, double now = std::numeric_limits<double>::quiet_NaN());
  using Field = std::pair<std::string_view, std::string_view>;
  void Event(std::string_view name, std::initializer_list<Field> fields = {}, double now = 0);
  void Event(std::string_view name, const std::vector<Field>& fields, double now = 0);
  void SetSession(const SessionReport& session);
  void SetPeer(const PeerReport& peer);
  std::optional<std::string> TakeBatch(double now, bool force = false);
  void RestoreBatch(const std::string& batch);
  void ClearReports();

  size_t pending_events() const { return events_.size(); }
  uint32_t dropped_events() const { return dropped_total_; }
  uint32_t rejected_names() const { return rejected_; }
  uint32_t unknown_reasons() const { return unknown_reasons_; }

 private:
  struct Stored {
    std::string json;
    bool retried = false;
  };
  std::string NormalizedReasonField(std::string_view value);
  std::string NormalizedResultField(std::string_view value);

  void EventImpl(std::string_view name, const Field* fields, size_t count, double now);
  void SetEpoch(double now);
  void Push(Stored event);
  void CountDropped(uint32_t n);
  void TrimQueue();

  TelemetryContext context_;
  bool have_context_ = false;
  bool have_epoch_ = false;
  double epoch_ = 0;

  std::deque<Stored> events_;
  std::optional<SessionReport> session_;
  std::map<std::pair<int, int>, PeerReport> peers_;
  bool dirty_ = false;

  bool has_last_batch_ = false;
  double last_batch_ = 0;

  uint32_t dropped_total_ = 0;
  uint32_t dropped_unreported_ = 0;
  uint32_t rejected_ = 0;
  uint32_t unknown_reasons_ = 0;
  uint32_t next_event_ = 0;

  // The newest batch handed out, so a failed delivery can put its events back (once).
  std::string last_text_;
  std::vector<Stored> last_events_;
  uint32_t last_dropped_ = 0;
};

}  // namespace tmnt::online
