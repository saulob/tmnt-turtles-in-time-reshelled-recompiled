#include "player_match_net.h"

#include <algorithm>
#include <cstring>

#include "guest_memory.h"
#include "player_match.h"

namespace tmnt {
namespace {

constexpr uint32_t kInaOffset = 0;
constexpr uint32_t kInaOnlineOffset = 4;
constexpr uint32_t kEnetOffset = 10;
constexpr uint32_t kOnlineIdOffset = 16;
constexpr uint8_t kXnAddrTag[4] = {'T', 'M', 'N', 'T'};
constexpr size_t kMaxQueued = 128;

// Neither of the two low bytes is ever 0 or 255, the values a subnet reserves.
uint32_t NextAddress(uint32_t address) {
  uint32_t hi = (address >> 8) & 0xFF;
  uint32_t lo = (address & 0xFF) + 1;
  if (lo > 254) {
    lo = 1;
    hi = hi % 254 + 1;
  }
  return kSyntheticNet | hi << 8 | lo;
}

}  // namespace

uint32_t SyntheticAddress(uint64_t xuid) {
  const uint32_t hi = 1 + uint32_t((xuid >> 8) & 0xFF) % 254;
  const uint32_t lo = 1 + uint32_t(xuid & 0xFF) % 254;
  return kSyntheticNet | hi << 8 | lo;
}

void SyntheticAddressBook::Update(std::vector<AddressMember> roster) {
  std::sort(roster.begin(), roster.end(), [](const AddressMember& a, const AddressMember& b) { return a.slot < b.slot; });
  for (auto it = assigned_.begin(); it != assigned_.end();) {
    const uint64_t xuid = it->first;
    const bool member = std::any_of(roster.begin(), roster.end(), [xuid](const AddressMember& m) { return m.xuid == xuid; });
    it = member ? std::next(it) : assigned_.erase(it);
  }
  const auto taken = [this](uint32_t address) {
    return std::any_of(assigned_.begin(), assigned_.end(), [address](const auto& entry) { return entry.second == address; });
  };
  for (const AddressMember& member : roster) {
    if (assigned_.count(member.xuid)) continue;
    uint32_t address = SyntheticAddress(member.xuid);
    while (taken(address)) address = NextAddress(address);
    assigned_[member.xuid] = address;
  }
}

uint32_t SyntheticAddressBook::AddressOf(uint64_t xuid) const {
  const auto found = assigned_.find(xuid);
  return found == assigned_.end() ? SyntheticAddress(xuid) : found->second;
}

void WriteXnAddr(uint8_t* xnaddr, uint64_t xuid) { WriteXnAddr(xnaddr, xuid, SyntheticAddress(xuid)); }

void WriteXnAddr(uint8_t* xnaddr, uint64_t xuid, uint32_t address) {
  std::memset(xnaddr, 0, kXnAddrBytes);
  StoreBE32(xnaddr, kInaOffset, address);
  StoreBE32(xnaddr, kInaOnlineOffset, address);
  for (uint32_t i = 0; i < 6; ++i) xnaddr[kEnetOffset + i] = uint8_t(xuid >> (40 - 8 * i));
  StoreBE64(xnaddr, kOnlineIdOffset, xuid);
  std::memcpy(xnaddr + kOnlineIdOffset + 8, kXnAddrTag, sizeof(kXnAddrTag));
}

std::optional<uint64_t> XnAddrXuid(const uint8_t* xnaddr) {
  if (std::memcmp(xnaddr + kOnlineIdOffset + 8, kXnAddrTag, sizeof(kXnAddrTag)) != 0) return std::nullopt;
  const uint64_t xuid = LoadBE64(xnaddr, kOnlineIdOffset);
  if (!IsSyntheticAddress(LoadBE32(xnaddr, kInaOffset))) return std::nullopt;
  return xuid;
}

uint32_t SessionTag(std::string_view match) {
  uint32_t hash = 0x811C9DC5u;
  for (const char c : match) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x01000193u;
  }
  return hash ? hash : 1;
}

std::optional<PeerRoute> RouteFor(const online::ActivePeer& peer, const SyntheticAddressBook& book) {
  if (peer.match.empty() || peer.player.empty() || peer.slot < 1 || peer.slot > online::kMaxParty) return std::nullopt;
  PeerRoute route;
  route.slot = peer.slot;
  route.xuid = VirtualXuid(peer.player);
  route.address = book.AddressOf(route.xuid);
  route.tag = SessionTag(peer.match);
  route.host = peer.host;
  return route;
}

std::vector<PeerRoute> RoutesFor(const std::vector<online::ActivePeer>& peers, int my_slot, uint64_t my_xuid,
                                 SyntheticAddressBook& book) {
  std::vector<AddressMember> roster;
  if (my_slot) roster.push_back({my_slot, my_xuid});
  for (const online::ActivePeer& peer : peers) roster.push_back({peer.slot, VirtualXuid(peer.player)});
  book.Update(std::move(roster));
  std::vector<PeerRoute> routes;
  for (const online::ActivePeer& peer : peers) {
    if (const auto route = RouteFor(peer, book)) routes.push_back(*route);
  }
  return routes;
}

const PeerRoute* RouteForAddress(const std::vector<PeerRoute>& routes, uint32_t address) {
  for (const PeerRoute& route : routes) {
    if (route.address == address) return &route;
  }
  return nullptr;
}

uint32_t QosResultBytes(const std::vector<QosTarget>& targets) {
  uint32_t bytes = kQosHeaderBytes + uint32_t(std::max<size_t>(targets.size(), 1)) * kQosInfoBytes;
  for (const QosTarget& t : targets) bytes += uint32_t(t.data.size());
  return bytes;
}

void WriteQosResult(uint8_t* out, uint32_t address, const std::vector<QosTarget>& targets) {
  std::memset(out, 0, QosResultBytes(targets));
  StoreBE32(out, 0, uint32_t(targets.size()));
  uint32_t data_at = kQosHeaderBytes + uint32_t(std::max<size_t>(targets.size(), 1)) * kQosInfoBytes;
  for (size_t i = 0; i < targets.size(); ++i) {
    const QosTarget& t = targets[i];
    uint8_t* info = out + kQosHeaderBytes + i * kQosInfoBytes;
    if (!t.reachable) {
      info[0] = kQosComplete | kQosTargetDisabled;
      continue;
    }
    info[0] = kQosComplete | kQosTargetContacted | (t.data.empty() ? 0 : kQosDataReceived);
    StoreBE16(info, 2, 1);
    StoreBE16(info, 4, 1);
    StoreBE16(info, 6, uint16_t(t.data.size()));
    StoreBE32(info, 8, t.data.empty() ? 0 : address + data_at);
    StoreBE16(info, 12, t.rtt_ms);
    StoreBE16(info, 14, t.rtt_ms);
    if (!t.data.empty()) std::memcpy(out + data_at, t.data.data(), t.data.size());
    data_at += uint32_t(t.data.size());
  }
}

QosTarget QosTargetFor(const uint8_t* xnaddr, const std::vector<online::PlayerMatchAd>& ads) {
  QosTarget target;
  const std::optional<uint64_t> xuid = XnAddrXuid(xnaddr);
  if (!xuid) return target;
  for (const online::PlayerMatchAd& ad : ads) {
    if (VirtualXuid(ad.player) != *xuid) continue;
    target.data = online::BytesOfHex(ad.qos);
    target.reachable = !target.data.empty();
    target.rtt_ms = kUnmeasuredRttMs;
    break;
  }
  return target;
}

std::vector<uint8_t> GatherBuffers(uint8_t* base, uint32_t buffers, uint32_t count) {
  std::vector<uint8_t> data;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t length = LoadBE32(GuestHost(base, buffers), i * 8);
    const uint32_t pointer = LoadBE32(GuestHost(base, buffers), i * 8 + 4);
    if (!length || !pointer) continue;
    const uint8_t* bytes = GuestHost(base, pointer);
    data.insert(data.end(), bytes, bytes + length);
  }
  return data;
}

uint32_t ScatterBuffers(uint8_t* base, uint32_t buffers, uint32_t count, const std::vector<uint8_t>& data) {
  uint32_t written = 0;
  for (uint32_t i = 0; i < count && written < data.size(); ++i) {
    const uint32_t length = LoadBE32(GuestHost(base, buffers), i * 8);
    const uint32_t pointer = LoadBE32(GuestHost(base, buffers), i * 8 + 4);
    if (!length || !pointer) continue;
    const uint32_t n = std::min<uint32_t>(length, uint32_t(data.size()) - written);
    std::memcpy(GuestHost(base, pointer), data.data() + written, n);
    written += n;
  }
  return written;
}

void PendingReceives::Add(const PendingReceive& receive) {
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](const PendingReceive& r) { return r.overlapped && r.overlapped == receive.overlapped; }),
                 pending_.end());
  pending_.push_back(receive);
}

std::optional<PendingReceive> PendingReceives::Take(uint32_t handle) {
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->handle != handle) continue;
    const PendingReceive receive = *it;
    pending_.erase(it);
    return receive;
  }
  return std::nullopt;
}

void PendingReceives::Drop(uint32_t handle) {
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [handle](const PendingReceive& r) { return r.handle == handle; }),
                 pending_.end());
}

bool PendingReceives::Has(uint32_t handle) const {
  return std::any_of(pending_.begin(), pending_.end(), [handle](const PendingReceive& r) { return r.handle == handle; });
}

std::vector<uint32_t> PendingReceives::handles() const {
  std::vector<uint32_t> out;
  for (const PendingReceive& r : pending_) {
    if (std::find(out.begin(), out.end(), r.handle) == out.end()) out.push_back(r.handle);
  }
  return out;
}

uint32_t VdpShim::Create() {
  const uint32_t handle = next_++;
  sockets_[handle] = {};
  return handle;
}

void VdpShim::Bind(uint32_t handle, uint16_t port) {
  if (auto it = sockets_.find(handle); it != sockets_.end()) it->second.port = port;
}

void VdpShim::Close(uint32_t handle) { sockets_.erase(handle); }

void VdpShim::Follow(const std::vector<PeerRoute>& routes) {
  const uint32_t tag = routes.empty() ? 0 : routes.front().tag;
  if (tag == tag_) return;
  tag_ = tag;
  for (auto& [handle, socket] : sockets_) socket.queue.clear();
}

bool VdpShim::SendTo(uint32_t handle, uint32_t to_address, uint16_t to_port, const uint8_t* data, size_t size,
                     const std::vector<PeerRoute>& routes, LobbyLink& link) {
  Follow(routes);
  const auto it = sockets_.find(handle);
  const PeerRoute* route = RouteForAddress(routes, to_address);
  if (it == sockets_.end() || !route) return false;
  netplay::LobbyDatagram datagram;
  datagram.from_port = it->second.port;
  datagram.to_port = to_port;
  datagram.data.assign(data, data + size);
  return link.Send(route->slot, datagram);
}

void VdpShim::TakeIn(const std::vector<PeerRoute>& routes, LobbyLink& link) {
  netplay::LobbyDatagram datagram;
  int slot = 0;
  while (link.Take(slot, datagram)) {
    const auto from = std::find_if(routes.begin(), routes.end(), [slot](const PeerRoute& r) { return r.slot == slot; });
    if (from == routes.end()) continue;
    Socket* target = nullptr;
    for (auto& [h, socket] : sockets_) {
      if (socket.port == datagram.to_port) target = &socket;
    }
    if (!target && sockets_.size() == 1) target = &sockets_.begin()->second;
    if (!target) continue;
    if (target->queue.size() < kMaxQueued) target->queue.push_back({from->address, std::move(datagram)});
    else ++dropped_;
  }
}

std::optional<VdpShim::Received> VdpShim::RecvFrom(uint32_t handle, const std::vector<PeerRoute>& routes, LobbyLink& link) {
  Follow(routes);
  if (routes.empty() || !Owns(handle)) return std::nullopt;
  TakeIn(routes, link);
  Socket& socket = sockets_[handle];
  if (socket.queue.empty()) return std::nullopt;
  Queued& front = socket.queue.front();
  Received received{front.from_address, front.datagram.from_port, std::move(front.datagram.data)};
  socket.queue.pop_front();
  return received;
}

bool VdpShim::Readable(uint32_t handle, const std::vector<PeerRoute>& routes, LobbyLink& link) {
  Follow(routes);
  if (routes.empty() || !Owns(handle)) return false;
  TakeIn(routes, link);
  return !sockets_[handle].queue.empty();
}

void VdpShim::Connect(uint32_t handle, uint32_t address, uint16_t port) {
  if (auto it = sockets_.find(handle); it != sockets_.end()) {
    it->second.connected = true;
    it->second.peer_address = address;
    it->second.peer_port = port;
  }
}

bool VdpShim::Peer(uint32_t handle, uint32_t& address, uint16_t& port) const {
  const auto it = sockets_.find(handle);
  if (it == sockets_.end() || !it->second.connected) return false;
  address = it->second.peer_address;
  port = it->second.peer_port;
  return true;
}

uint16_t VdpShim::bound_port(uint32_t handle) const {
  const auto it = sockets_.find(handle);
  return it == sockets_.end() ? 0 : it->second.port;
}

size_t VdpShim::deepest_queue() const {
  size_t deepest = 0;
  for (const auto& [handle, socket] : sockets_) deepest = std::max(deepest, socket.queue.size());
  return deepest;
}

size_t VdpShim::queued(uint32_t handle) const {
  const auto it = sockets_.find(handle);
  return it == sockets_.end() ? 0 : it->second.queue.size();
}

}  // namespace tmnt
