#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "online/identity.h"
#include "online/stun.h"
#include "online/upnp.h"

namespace tmnt::online {

enum class Connection { kOff, kChecking, kOnline, kOffline, kError };

struct ConnectionFacts {
  enum class Router { kNone, kMapped, kMappedBehindNat };
  bool identity_ok = true;
  bool lobby_ok = false;
  std::string lobby_refusal;     // the service answered but refused this game; shown instead of "cannot reach"
  int64_t lobby_latency_ms = -1;  // the sign-in's round trip to the online service, not a player's Ping
  bool stun_off = false;
  bool udp_ok = false;          // a STUN server answered
  bool public_address = false;  // the STUN address is this PC's own
  bool second_answer = false;   // a second STUN server answered
  bool same_mapping = false;    // and saw the same public port
  Router router = Router::kNone;
  uint16_t udp_port = 0;     // the multiplayer port the test opened; 0: none, or any free one when none was asked
  std::string closed_ports;  // the multiplayer ports it could not open ("7001 or 7002"); empty when it opened one
  bool relay_ok = false;     // the service relays a match that cannot connect directly
  std::string worker_status = "unreachable";  // ok, unreachable, bad_status, unrecognized, protocol_mismatch
  std::string ws_status = "skipped";          // ok, failed, refused, skipped
  int service_protocol = -1;
};

struct ConnectionReport {
  enum class Mark { kOk, kNote, kProblem };
  struct Line {
    Mark mark = Mark::kProblem;
    std::string label;
    std::string text;
  };
  std::vector<Line> lines;  // plain language, no addresses; when ready only what makes it ready
  bool worker_reachable = false;
  bool stun_ok = false;
  bool direct_available = false;
  bool relay_available = false;
  bool overall_usable = false;  // what PLAY waits for; Direct or Relay is enough
  bool ready = false;           // overall_usable
  std::string verdict;
  std::string worker_status, ws_status, stun_status, router_kind;  // short codes for the support beacon
  std::string failure;  // a connection_test reason code, empty when overall usable
  int service_protocol = -1;
  int latency_ms = -1;
  std::vector<std::string> details;  // addresses, servers and errors, for the log
};

ConnectionReport SummarizeConnection(const ConnectionFacts& facts);

// The query string of GET /telemetry/test: the outcome of one Connection Test, with no address or secret in it.
std::string ConnectionTestBeaconQuery(const ConnectionReport& report, const std::string& diag, const std::string& version,
                                      const std::string& platform, const std::string& os, const std::string& arch);

// A router that accepted the UPnP mapping and reports a private, carrier-grade or empty (0.0.0.0) address has another NAT
// in front of it.
ConnectionFacts::Router ClassifyRouter(uint32_t router_ip);

struct ConnectionTestRequest {
  std::string url;
  Identity identity;
  std::string name;
  std::string guest;
  int build = 1;
  std::string stun = net::kDefaultStunServers;
  bool upnp = true;
  // STUN and UPnP use one socket bound to the first of these that is free, as the game's socket is, and never
  // another port. Empty: any free port.
  std::vector<uint16_t> game_ports = {7001, 7002, 7003, 7004};
  // The port the game's own socket already holds: STUN then runs beside it, and the report and UPnP are about this one.
  uint16_t game_port = 0;
  std::function<net::upnp::Status(uint16_t port)> probe_upnp = net::upnp::Probe;
};

// Blocks for a few seconds (service, two STUN servers, the router); run it off the UI thread. Its socket is closed
// when it returns.
ConnectionReport RunConnectionTest(const ConnectionTestRequest& request);

}  // namespace tmnt::online
