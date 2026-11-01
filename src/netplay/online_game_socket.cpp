#include "netplay/online_game_socket.h"

#include <algorithm>
#include <cstring>

namespace tmnt::netplay {
namespace {

constexpr size_t kMaxDatagram = 2048;
constexpr size_t kMaxWaiting = 64;
// About four seconds of a match's traffic: a consumer this far behind has stalled.
constexpr size_t kMaxRelayQueued = 256;

void Sample(std::vector<size_t>& sizes, size_t size) {
  if (sizes.size() < kRelaySampledSizes) sizes.push_back(size);
}

}  // namespace

bool OnlineGameSocket::Open(std::initializer_list<uint16_t> ports, bool loopback_only) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (socket_.is_open()) return true;
  for (const uint16_t port : ports) {
    if (socket_.Open(port, loopback_only)) return true;
  }
  return false;
}

bool OnlineGameSocket::is_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return socket_.is_open();
}

uint16_t OnlineGameSocket::bound_port() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return socket_.bound_port();
}

void OnlineGameSocket::SetTap(DatagramTap* tap) {
  std::lock_guard<std::mutex> lock(mutex_);
  tap_ = tap;
}

void OnlineGameSocket::Use(const std::function<void(Transport&)>& use) {
  std::lock_guard<std::mutex> lock(mutex_);
  use(socket_);
}

void OnlineGameSocket::Pump() {
  std::lock_guard<std::mutex> lock(mutex_);
  uint8_t buffer[kMaxDatagram];
  Endpoint from;
  while (const size_t size = socket_.Receive(buffer, sizeof buffer, from)) {
    if (direct_filter_ && !direct_filter_(from)) continue;
    if (TakenByTap(from, buffer, size)) continue;
    if (waiting_.size() == kMaxWaiting) waiting_.pop_front();
    waiting_.emplace_back(from, std::vector<uint8_t>(buffer, buffer + size));
  }
}

void OnlineGameSocket::OpenRelay(uint32_t token, int my_slot, RelaySend send) {
  std::lock_guard<std::mutex> lock(mutex_);
  relay_token_ = token;
  relay_slot_ = my_slot;
  relay_send_ = std::move(send);
  relay_peers_ = {};
  next_relay_peer_ = 1;
}

void OnlineGameSocket::SelectRelay(int peer_slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (relay_token_ && peer_slot >= 1 && peer_slot <= kMaxSlot) relay_peers_[peer_slot].selected = true;
}

void OnlineGameSocket::CloseRelayPeer(int peer_slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (peer_slot >= 1 && peer_slot <= kMaxSlot) relay_peers_[peer_slot] = {};
}

void OnlineGameSocket::CloseRelay() {
  std::lock_guard<std::mutex> lock(mutex_);
  relay_token_ = 0;
  relay_slot_ = 0;
  relay_send_ = nullptr;
  relay_peers_ = {};
}

bool OnlineGameSocket::relay_open() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return relay_token_ != 0;
}

bool OnlineGameSocket::relay_selected(int peer_slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_slot >= 1 && peer_slot <= kMaxSlot && relay_peers_[peer_slot].selected;
}

bool OnlineGameSocket::DeliverRelay(const uint8_t* data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<RelayFrame> frame = DecodeRelayFrame(data, size);
  if (!frame || !relay_token_ || frame->token != relay_token_ || frame->dst != relay_slot_ || frame->src == relay_slot_) {
    ++traffic_.relay_rejected;
    if (frame && frame->src >= 1 && frame->src <= kMaxSlot) ++peer_traffic_[frame->src].relay_rejected;
    return false;
  }
  PeerRelay& peer = relay_peers_[frame->src];
  if (!peer.failure.empty()) {
    ++traffic_.relay_rejected;
    ++peer_traffic_[frame->src].relay_rejected;
    return false;
  }
  if (peer.in.size() >= kMaxRelayQueued) {
    peer.failure = "receive_overflow";
    peer.in.clear();
    return false;
  }
  ++traffic_.relay_rx_packets;
  ++peer_traffic_[frame->src].relay_rx_packets;
  traffic_.relay_rx_bytes += frame->payload.size();
  Sample(traffic_.relay_rx_sizes, frame->payload.size());
  peer.in.push_back(std::move(frame->payload));
  return true;
}

std::string OnlineGameSocket::relay_failure(int peer_slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_slot >= 1 && peer_slot <= kMaxSlot ? relay_peers_[peer_slot].failure : std::string();
}

void OnlineGameSocket::SetDirectFilter(std::function<bool(const Endpoint&)> allow) {
  std::lock_guard<std::mutex> lock(mutex_);
  direct_filter_ = std::move(allow);
}

void OnlineGameSocket::SetPeerEndpoint(int peer_slot, const Endpoint& endpoint) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (peer_slot >= 1 && peer_slot <= kMaxSlot) peer_endpoints_[peer_slot] = endpoint;
}

int OnlineGameSocket::PeerOfEndpoint(const Endpoint& endpoint) const {
  for (int slot = 1; slot <= kMaxSlot; ++slot) {
    if (peer_endpoints_[slot].port && peer_endpoints_[slot] == endpoint) return slot;
  }
  return 0;
}

void OnlineGameSocket::ResetTraffic() {
  std::lock_guard<std::mutex> lock(mutex_);
  traffic_ = {};
  peer_traffic_ = {};
}

MatchTraffic OnlineGameSocket::traffic() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return traffic_;
}

PeerTraffic OnlineGameSocket::peer_traffic(int peer_slot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peer_slot >= 1 && peer_slot <= kMaxSlot ? peer_traffic_[peer_slot] : PeerTraffic{};
}

bool OnlineGameSocket::SendRelayLocked(int peer_slot, const uint8_t* data, size_t size) {
  PeerRelay& peer = relay_peers_[peer_slot];
  if (!peer.failure.empty() || !relay_send_) return false;
  std::vector<uint8_t> frame =
      EncodeRelayFrame(uint8_t(relay_slot_), uint8_t(peer_slot), relay_token_, ++peer.seq, data, size);
  if (frame.empty()) return false;
  if (!relay_send_(std::move(frame))) {
    peer.failure = "send_refused";
    return false;
  }
  ++traffic_.relay_tx_packets;
  ++peer_traffic_[peer_slot].relay_tx_packets;
  traffic_.relay_tx_bytes += size;
  Sample(traffic_.relay_tx_sizes, size);
  return true;
}

bool OnlineGameSocket::Send(const Endpoint& to, const uint8_t* data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (const int slot = RelayPeerSlot(to); slot && relay_peers_[slot].selected) return SendRelayLocked(slot, data, size);
  if (direct_filter_ && !direct_filter_(to)) return true;
  const bool sent = socket_.Send(to, data, size);
  if (sent) {
    ++traffic_.udp_tx_packets;
    if (const int slot = PeerOfEndpoint(to)) ++peer_traffic_[slot].udp_tx_packets;
  }
  return sent;
}

size_t OnlineGameSocket::Receive(uint8_t* buffer, size_t capacity, Endpoint& from) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < kMaxSlot * 64; ++i) {
    const int slot = (next_relay_peer_ - 1 + i) % kMaxSlot + 1;
    PeerRelay& peer = relay_peers_[slot];
    if (!peer.selected || peer.in.empty()) continue;
    std::vector<uint8_t> data = std::move(peer.in.front());
    peer.in.pop_front();
    next_relay_peer_ = slot % kMaxSlot + 1;
    RelayReply reply(*this, slot);
    if (TapTakesRelayed(RelayPeerEndpoint(slot), data, reply)) continue;
    const size_t size = std::min(capacity, data.size());
    std::memcpy(buffer, data.data(), size);
    from = RelayPeerEndpoint(slot);
    return size;
  }
  const auto count_udp_rx = [this](const Endpoint& sender) {
    ++traffic_.udp_rx_packets;
    if (const int slot = PeerOfEndpoint(sender)) ++peer_traffic_[slot].udp_rx_packets;
  };
  if (!waiting_.empty()) {
    const auto& [sender, data] = waiting_.front();
    const size_t size = std::min(capacity, data.size());
    if (size) std::memcpy(buffer, data.data(), size);
    from = sender;
    waiting_.pop_front();
    count_udp_rx(from);
    return size;
  }
  for (;;) {
    const size_t size = socket_.Receive(buffer, capacity, from);
    if (size == 0) return 0;
    if (direct_filter_ && !direct_filter_(from)) continue;
    if (!TakenByTap(from, buffer, size)) {
      count_udp_rx(from);
      return size;
    }
  }
}

bool OnlineGameSocket::TapTakesRelayed(const Endpoint& from, const std::vector<uint8_t>& data, Transport& reply) {
  return tap_ && tap_->Take(from, data.data(), data.size(), reply);
}

bool OnlineGameSocket::TakenByTap(const Endpoint& from, const uint8_t* data, size_t size) {
  return tap_ && tap_->Take(from, data, size, socket_);
}

}  // namespace tmnt::netplay
