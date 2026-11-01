// The title's XNet, QoS and socket wrappers while an online match can run: peers are synthetic XNet addresses, and
// the title's datagram sockets are shim handles whose traffic goes through online_bridge.cpp.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <atomic>
#include <optional>
#include <random>
#include <string>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xevent.h>

#include "guest_memory.h"
#include "online_bridge.h"
#include "online_runtime.h"
#include "player_match.h"
#include "player_match_net.h"
#include "socket_select.h"

namespace {

using tmnt::GuestHost;
using tmnt::LoadBE16;
using tmnt::LoadBE32;
using tmnt::StoreBE16;
using tmnt::StoreBE32;

bool LiveNow() { return tmnt::OnlineRunning() && tmnt::Online().Profile().signed_in; }

std::vector<tmnt::PeerRoute> ActiveRoutes() { return tmnt::RoutesFor(tmnt::Online().Peers()); }

std::string Dotted(uint32_t a) {
  char text[16];
  std::snprintf(text, sizeof text, "%u.%u.%u.%u", a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
  return text;
}

// DEBUG_SAULO: the first datagrams each way, then one line per 256.
bool ShouldLogPacket(uint32_t count) { return count <= 8 || count % 256 == 0; }

class MuxLink final : public tmnt::LobbyLink {
 public:
  bool Send(int slot, const tmnt::netplay::LobbyDatagram& d) override { return tmnt::SendPlayerMatchLobby(slot, d); }
  bool Take(int& slot, tmnt::netplay::LobbyDatagram& d) override { return tmnt::TakePlayerMatchLobby(slot, d); }
};

std::mutex g_shim_mutex;
tmnt::VdpShim g_shim;
tmnt::PendingReceives g_pending;
MuxLink g_link;
thread_local uint32_t g_pending_error = 0;
uint32_t g_sent = 0;
uint32_t g_received = 0;
uint32_t g_mapped_address = 0;

bool OwnsSocket(uint32_t handle) {
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  return g_shim.Owns(handle);
}

void SignalEvent(uint32_t handle) {
  if (!handle) return;
  if (auto event = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle)) event->Set(0, false);
}

void WriteFrom(uint8_t* base, uint32_t from, uint32_t from_length, uint32_t address, uint16_t port) {
  if (from) {
    uint8_t* sockaddr = GuestHost(base, from);
    std::memset(sockaddr, 0, 16);
    StoreBE16(sockaddr, 0, 2);
    StoreBE16(sockaddr, 2, port);
    StoreBE32(sockaddr, 4, address);
  }
  if (from_length) StoreBE32(GuestHost(base, from_length), 0, 16);
}

// Fills the receive's buffers and its overlapped as a finished receive would. Caller holds g_shim_mutex.
uint32_t DeliverLocked(uint8_t* base, const tmnt::PendingReceive& r, const tmnt::VdpShim::Received& received) {
  const uint32_t size = tmnt::ScatterBuffers(base, r.buffers, r.buffer_count, received.data);
  WriteFrom(base, r.from, r.from_length, received.from_address, received.from_port);
  if (r.bytes) StoreBE32(GuestHost(base, r.bytes), 0, size);
  if (r.overlapped) {
    uint8_t* overlapped = GuestHost(base, r.overlapped);
    StoreBE32(overlapped, tmnt::kOverlappedStatus, 0);
    StoreBE32(overlapped, tmnt::kOverlappedBytes, size);
  }
  if (ShouldLogPacket(++g_received)) {
    REXLOG_INFO("DEBUG_SAULO player_match packet rx n={} handle={:08X} from={}:{} bytes={}", g_received, r.handle,
                Dotted(received.from_address), received.from_port, size);
  }
  return size;
}

void XnAddrToInAddr(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t xnaddr = ctx.r3.u32;
  const uint32_t out = ctx.r5.u32;
  const auto xuid = xnaddr && out && LiveNow() ? tmnt::XnAddrXuid(GuestHost(base, xnaddr)) : std::nullopt;
  if (!xuid) {
    original(ctx, base);
    return;
  }
  const uint32_t address = tmnt::SyntheticAddress(*xuid);
  StoreBE32(GuestHost(base, out), 0, address);
  ctx.r3.u64 = 0;
  if (address != g_mapped_address) {
    g_mapped_address = address;
    REXLOG_INFO("DEBUG_SAULO player_match xnet map created xuid={:016X} address={}", *xuid, Dotted(address));
  }
}

void XNetCreateKey(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t id = ctx.r3.u32;
  const uint32_t key = ctx.r4.u32;
  if (!id || !key || !LiveNow()) {
    original(ctx, base);
    return;
  }
  uint8_t kid[tmnt::kNetKeyIdBytes], exchange[tmnt::kNetKeyBytes];
  std::random_device entropy;
  tmnt::MakeNetKey((uint64_t(entropy()) << 32) | entropy(), kid, exchange);
  std::memcpy(GuestHost(base, id), kid, sizeof kid);
  std::memcpy(GuestHost(base, key), exchange, sizeof exchange);
  ctx.r3.u64 = 0;
  REXLOG_INFO("DEBUG_SAULO player_match XNetCreateKey id_prefix={:02X}{:02X} ok", kid[0], kid[1]);
}

void XNetKeyRegistration(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (!LiveNow()) {
    original(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// The SDK reports no link at all; an online console has an active, full-duplex 100 Mbps one.
void EthernetLinkStatus(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kLinkActive100MbpsFullDuplex = 0x1 | 0x2 | 0x8;
  if (!LiveNow()) {
    original(ctx, base);
    return;
  }
  ctx.r3.u64 = kLinkActive100MbpsFullDuplex;
}

void QosListen(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kSetData = 0x4;
  const uint32_t data = ctx.r4.u32;
  const uint32_t size = ctx.r5.u32;
  const uint32_t flags = ctx.r7.u32;
  if ((flags & kSetData) && data && size && size <= 512 && LiveNow()) {
    const uint8_t* bytes = GuestHost(base, data);
    tmnt::Online().SetPlayerMatchQos(std::vector<uint8_t>(bytes, bytes + size));
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

void QosLookup(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t count = ctx.r3.u32;
  const uint32_t xnaddrs = ctx.r4.u32;
  const uint32_t stack = ctx.r1.u32;
  const uint32_t event = LoadBE32(base, stack + 100);
  const uint32_t result_ptr = LoadBE32(base, stack + 108);
  if (!LiveNow() || !xnaddrs || !result_ptr || count == 0 || count > 64) {
    original(ctx, base);
    return;
  }
  const std::vector<tmnt::online::PlayerMatchAd> ads = tmnt::Online().PlayerMatchResults();
  std::vector<tmnt::QosTarget> targets;
  bool ours = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t xnaddr = LoadBE32(GuestHost(base, xnaddrs), i * 4);
    if (!xnaddr) {
      targets.emplace_back();
      continue;
    }
    ours = ours || tmnt::XnAddrXuid(GuestHost(base, xnaddr)).has_value();
    targets.push_back(tmnt::QosTargetFor(GuestHost(base, xnaddr), ads));
  }
  if (!ours) {
    original(ctx, base);
    return;
  }
  const uint32_t bytes = tmnt::QosResultBytes(targets);
  const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(bytes);
  if (!address) {
    original(ctx, base);
    return;
  }
  tmnt::WriteQosResult(GuestHost(base, address), address, targets);
  StoreBE32(GuestHost(base, result_ptr), 0, address);
  SignalEvent(event);
  ctx.r3.u64 = 0;
  REXLOG_INFO("DEBUG_SAULO player_match qos complete targets={}", count);
}

void Socket(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t type = ctx.r4.u32;
  const uint32_t protocol = ctx.r5.u32;
  if (!tmnt::ShimSocket(type, protocol) || !LiveNow()) {
    original(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  ctx.r3.u64 = g_shim.Create();
  REXLOG_INFO("DEBUG_SAULO player_match socket create handle={:08X} type={} protocol={}", ctx.r3.u32, type, protocol);
}

void Bind(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  const uint16_t port = ctx.r4.u32 ? LoadBE16(GuestHost(base, ctx.r4.u32), 2) : 0;
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  g_shim.Bind(handle, port);
  ctx.r3.u64 = 0;
  REXLOG_INFO("DEBUG_SAULO player_match socket bind handle={:08X} port={}", handle, port);
}

void GetSockName(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  uint16_t port = 0;
  {
    std::lock_guard<std::mutex> lock(g_shim_mutex);
    port = g_shim.bound_port(handle);
  }
  WriteFrom(base, ctx.r4.u32, ctx.r5.u32, 0, port);
  ctx.r3.u64 = 0;
}

// ioctlsocket and setsockopt have nothing to change on a shim socket.
void Succeed(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (!OwnsSocket(ctx.r3.u32)) {
    original(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

void Close(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  constexpr uint32_t kStatusCancelled = 0xC0000120;
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  g_shim.Close(handle);
  while (const std::optional<tmnt::PendingReceive> aborted = g_pending.Take(handle)) {
    if (!aborted->overlapped) continue;
    StoreBE32(GuestHost(base, aborted->overlapped), tmnt::kOverlappedStatus, kStatusCancelled);
    SignalEvent(LoadBE32(GuestHost(base, aborted->overlapped), tmnt::kOverlappedEvent));
  }
  ctx.r3.u64 = 0;
  REXLOG_INFO("DEBUG_SAULO player_match socket close handle={:08X}", handle);
}

bool RouteTo(uint32_t handle, uint32_t address, uint16_t port, const std::vector<uint8_t>& data) {
  const std::vector<tmnt::PeerRoute> routes = ActiveRoutes();
  bool routed = false;
  {
    std::lock_guard<std::mutex> lock(g_shim_mutex);
    routed = g_shim.SendTo(handle, address, port, data.data(), data.size(), routes, g_link);
  }
  if (ShouldLogPacket(++g_sent)) {
    REXLOG_INFO("DEBUG_SAULO player_match packet tx n={} handle={:08X} to={}:{} bytes={} routed={}", g_sent, handle,
                Dotted(address), port, data.size(), routed);
  }
  return routed;
}

bool Route(uint32_t handle, uint32_t to, const std::vector<uint8_t>& data, uint8_t* base) {
  const uint32_t address = to ? LoadBE32(GuestHost(base, to), 4) : 0;
  const uint16_t port = to ? LoadBE16(GuestHost(base, to), 2) : 0;
  return RouteTo(handle, address, port, data);
}

void SendTo(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  const uint8_t* bytes = GuestHost(base, ctx.r4.u32);
  const std::vector<uint8_t> data(bytes, bytes + ctx.r5.u32);
  Route(handle, ctx.r7.u32, data, base);
  ctx.r3.u64 = data.size();
}

// A datagram socket's send never waits: it completes at once, overlapped or not.
void WsaSendTo(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  const std::vector<uint8_t> data = tmnt::GatherBuffers(base, ctx.r4.u32, ctx.r5.u32);
  Route(handle, ctx.r8.u32, data, base);
  if (ctx.r6.u32) StoreBE32(GuestHost(base, ctx.r6.u32), 0, uint32_t(data.size()));
  if (const uint32_t overlapped = ctx.r10.u32) {
    StoreBE32(GuestHost(base, overlapped), tmnt::kOverlappedStatus, 0);
    StoreBE32(GuestHost(base, overlapped), tmnt::kOverlappedBytes, uint32_t(data.size()));
    SignalEvent(LoadBE32(GuestHost(base, overlapped), tmnt::kOverlappedEvent));
  }
  ctx.r3.u64 = 0;
}

void RecvFrom(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  const std::vector<tmnt::PeerRoute> routes = ActiveRoutes();
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  const std::optional<tmnt::VdpShim::Received> received = g_shim.RecvFrom(handle, routes, g_link);
  if (!received) {
    g_pending_error = tmnt::kWouldBlock;
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  const uint32_t size = std::min<uint32_t>(ctx.r5.u32, uint32_t(received->data.size()));
  std::memcpy(GuestHost(base, ctx.r4.u32), received->data.data(), size);
  WriteFrom(base, ctx.r7.u32, ctx.r8.u32, received->from_address, received->from_port);
  ctx.r3.u64 = size;
}

// With an overlapped and nothing waiting, the receive stays pending and the network pump completes it (its event is
// set, and WSAGetOverlappedResult reports it) when the next datagram for the socket arrives.
void WsaRecvFrom(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  tmnt::PendingReceive receive;
  receive.handle = handle;
  receive.buffers = ctx.r4.u32;
  receive.buffer_count = ctx.r5.u32;
  receive.bytes = ctx.r6.u32;
  receive.from = ctx.r8.u32;
  receive.from_length = ctx.r9.u32;
  receive.overlapped = ctx.r10.u32;
  if (ctx.r7.u32) StoreBE32(GuestHost(base, ctx.r7.u32), 0, 0);
  const std::vector<tmnt::PeerRoute> routes = ActiveRoutes();
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  if (!g_pending.Has(handle)) {
    if (const auto received = g_shim.RecvFrom(handle, routes, g_link)) {
      DeliverLocked(base, receive, *received);
      if (receive.overlapped) SignalEvent(LoadBE32(GuestHost(base, receive.overlapped), tmnt::kOverlappedEvent));
      ctx.r3.u64 = 0;
      return;
    }
  }
  if (!receive.overlapped) {
    g_pending_error = tmnt::kWouldBlock;
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  StoreBE32(GuestHost(base, receive.overlapped), tmnt::kOverlappedStatus, tmnt::kStatusPending);
  receive.bytes = 0;
  g_pending.Add(receive);
  g_pending_error = tmnt::kIoPending;
  ctx.r3.u64 = 0xFFFFFFFFu;
}

void GetOverlappedResult(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  const uint32_t overlapped = ctx.r4.u32;
  if (!overlapped || !OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  tmnt::CompleteVdpReceives(base);
  if (LoadBE32(GuestHost(base, overlapped), tmnt::kOverlappedStatus) == tmnt::kStatusPending) {
    g_pending_error = tmnt::kIoIncomplete;
    ctx.r3.u64 = 0;
    return;
  }
  if (ctx.r5.u32) {
    StoreBE32(GuestHost(base, ctx.r5.u32), 0, LoadBE32(GuestHost(base, overlapped), tmnt::kOverlappedBytes));
  }
  if (ctx.r7.u32) StoreBE32(GuestHost(base, ctx.r7.u32), 0, 0);
  ctx.r3.u64 = 1;
}

void LastError(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  if (g_pending_error) {
    ctx.r3.u64 = g_pending_error;
    g_pending_error = 0;
    return;
  }
  original(ctx, base);
}

// ReXGlue's WSAWaitForMultipleEvents passes fWaitAll as the NT wait type (0 means all) and always returns 0.
void WsaWaitForEvents(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kMaxEvents = 64;
  constexpr uint32_t kWaitTimeout = 258;
  constexpr uint32_t kWaitIoCompletion = 192;
  constexpr uint32_t kWaitFailed = 0xFFFFFFFF;
  constexpr uint32_t kInfinite = 0xFFFFFFFF;
  constexpr uint32_t kStatusTimeout = 0x102;
  constexpr uint32_t kStatusUserApc = 0xC0;
  const uint32_t count = ctx.r3.u32;
  const uint32_t events = ctx.r4.u32;
  if (!LiveNow() || !events || !count || count > kMaxEvents) {
    original(ctx, base);
    return;
  }
  std::vector<rex::system::object_ref<rex::system::XObject>> held;
  std::vector<rex::system::XObject*> objects;
  for (uint32_t i = 0; i < count; ++i) {
    auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(LoadBE32(GuestHost(base, events), i * 4));
    if (!object) {
      original(ctx, base);
      return;
    }
    objects.push_back(object.get());
    held.push_back(std::move(object));
  }
  const bool wait_all = ctx.r5.u32 != 0;
  const uint32_t milliseconds = ctx.r6.u32;
  // Receives complete from the network pump, which a thread blocked here would starve; take in datagrams between
  // short waits instead.
  const auto started = std::chrono::steady_clock::now();
  rex::X_STATUS status = kStatusTimeout;
  for (bool first = true;; first = false) {
    tmnt::CompleteVdpReceives(base);
    const uint32_t elapsed = uint32_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
    if (tmnt::WaitExpired(milliseconds, elapsed, first)) break;
    uint64_t relative = uint64_t(-(int64_t(tmnt::NextWaitSlice(milliseconds, elapsed)) * 10000));
    status = rex::system::XObject::WaitMultiple(count, objects.data(), wait_all ? 0 : 1, 6, 1, ctx.r7.u32 ? 1 : 0,
                                                &relative);
    if (status != kStatusTimeout) break;
  }
  if (status == kStatusTimeout) ctx.r3.u64 = kWaitTimeout;
  else if (status == kStatusUserApc) ctx.r3.u64 = kWaitIoCompletion;
  else if (status < count) ctx.r3.u64 = status;
  else ctx.r3.u64 = kWaitFailed;
}

void Connect(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  if (const uint32_t to = ctx.r4.u32) {
    std::lock_guard<std::mutex> lock(g_shim_mutex);
    g_shim.Connect(handle, LoadBE32(GuestHost(base, to), 4), LoadBE16(GuestHost(base, to), 2));
  }
  ctx.r3.u64 = 0;
}

void Send(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  constexpr uint32_t kNotConnected = 10057;
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  uint32_t address = 0;
  uint16_t port = 0;
  bool connected = false;
  {
    std::lock_guard<std::mutex> lock(g_shim_mutex);
    connected = g_shim.Peer(handle, address, port);
  }
  if (!connected) {
    g_pending_error = kNotConnected;
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  const uint8_t* bytes = GuestHost(base, ctx.r4.u32);
  const std::vector<uint8_t> data(bytes, bytes + ctx.r5.u32);
  RouteTo(handle, address, port, data);
  ctx.r3.u64 = data.size();
}

void Recv(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t handle = ctx.r3.u32;
  if (!OwnsSocket(handle)) {
    original(ctx, base);
    return;
  }
  const std::vector<tmnt::PeerRoute> routes = ActiveRoutes();
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  const std::optional<tmnt::VdpShim::Received> received = g_shim.RecvFrom(handle, routes, g_link);
  if (!received) {
    g_pending_error = tmnt::kWouldBlock;
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  const uint32_t size = std::min<uint32_t>(ctx.r5.u32, uint32_t(received->data.size()));
  std::memcpy(GuestHost(base, ctx.r4.u32), received->data.data(), size);
  ctx.r3.u64 = size;
}

constexpr uint32_t kFdSetMaxHandles = 64;

std::vector<uint32_t> FdHandles(uint8_t* base, uint32_t set) {
  std::vector<uint32_t> handles;
  if (!set) return handles;
  const uint32_t count = std::min(LoadBE32(GuestHost(base, set), 0), kFdSetMaxHandles);
  for (uint32_t i = 0; i < count; ++i) handles.push_back(LoadBE32(GuestHost(base, set), 4 + i * 4));
  return handles;
}

void StoreFdHandles(uint8_t* base, uint32_t set, const std::vector<uint32_t>& handles) {
  if (!set) return;
  StoreBE32(GuestHost(base, set), 0, uint32_t(handles.size()));
  for (size_t i = 0; i < handles.size(); ++i) StoreBE32(GuestHost(base, set), 4 + uint32_t(i) * 4, handles[i]);
}

// A guest timeval that stays zero, for polling a real select when the caller passed no timeout.
uint32_t PollTimeoutScratch() {
  static const uint32_t address = REX_KERNEL_MEMORY()->SystemHeapAlloc(8);
  return address;
}

// Host Winsock fails select on a handle it does not know: shim sockets are answered from the shim, real ones by the
// original select, polled without waiting.
void Select(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t read_set = ctx.r4.u32;
  const uint32_t write_set = ctx.r5.u32;
  const uint32_t except_set = ctx.r6.u32;
  const uint32_t timeout_pointer = ctx.r7.u32;
  tmnt::FdSets requested{FdHandles(base, read_set), FdHandles(base, write_set), FdHandles(base, except_set)};
  const auto owned = [](uint32_t handle) { return OwnsSocket(handle); };
  const bool any_shim = std::any_of(requested.read.begin(), requested.read.end(), owned) ||
                        std::any_of(requested.write.begin(), requested.write.end(), owned) ||
                        std::any_of(requested.except.begin(), requested.except.end(), owned);
  if (!any_shim) {
    original(ctx, base);
    return;
  }
  tmnt::SelectTimeout timeout;
  if (!timeout_pointer) {
    timeout.infinite = true;
  } else {
    const uint32_t seconds = LoadBE32(GuestHost(base, timeout_pointer), 0);
    const uint32_t microseconds = LoadBE32(GuestHost(base, timeout_pointer), 4);
    timeout.milliseconds = seconds * 1000 + microseconds / 1000;
  }
  const PPCContext saved = ctx;
  uint8_t saved_timeout[8] = {};
  if (timeout_pointer) std::memcpy(saved_timeout, GuestHost(base, timeout_pointer), sizeof saved_timeout);
  const uint32_t zero_timeout = timeout_pointer ? timeout_pointer : PollTimeoutScratch();
  const auto real_select = [&](tmnt::FdSets& sets) {
    StoreFdHandles(base, read_set, sets.read);
    StoreFdHandles(base, write_set, sets.write);
    StoreFdHandles(base, except_set, sets.except);
    std::memset(GuestHost(base, zero_timeout), 0, sizeof saved_timeout);
    ctx = saved;
    ctx.r7.u64 = zero_timeout;
    original(ctx, base);
    const int ready = ctx.r3.s32;
    sets = {FdHandles(base, read_set), FdHandles(base, write_set), FdHandles(base, except_set)};
    return ready;
  };
  const auto readable = [&](uint32_t handle) {
    const std::vector<tmnt::PeerRoute> routes = ActiveRoutes();
    std::lock_guard<std::mutex> lock(g_shim_mutex);
    return g_shim.Readable(handle, routes, g_link);
  };
  const auto now = [] {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
  };
  const auto pause = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
  const tmnt::SelectOutcome outcome = tmnt::SelectMixed(requested, owned, readable, real_select, timeout, now, pause);
  if (timeout_pointer) std::memcpy(GuestHost(base, timeout_pointer), saved_timeout, sizeof saved_timeout);
  ctx = saved;
  StoreFdHandles(base, read_set, outcome.ready.read);
  StoreFdHandles(base, write_set, outcome.ready.write);
  StoreFdHandles(base, except_set, outcome.ready.except);
  ctx.r3.u64 = uint32_t(outcome.count);
}

}  // namespace

namespace tmnt {

void CompleteVdpReceives(uint8_t* base) {
  const std::vector<PeerRoute> routes = ActiveRoutes();
  std::lock_guard<std::mutex> lock(g_shim_mutex);
  for (const uint32_t handle : g_pending.handles()) {
    while (g_pending.Has(handle)) {
      auto received = g_shim.RecvFrom(handle, routes, g_link);
      if (!received) break;
      const PendingReceive receive = *g_pending.Take(handle);
      DeliverLocked(base, receive, *received);
      SignalEvent(LoadBE32(GuestHost(base, receive.overlapped), kOverlappedEvent));
    }
  }
}

}  // namespace tmnt

#define TMNT_NET_HOOK(address, handler)        \
  REX_EXTERN(__imp__sub_##address);            \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

TMNT_NET_HOOK(823CB3E0, XNetCreateKey)
TMNT_NET_HOOK(823CB3F0, XNetKeyRegistration)
TMNT_NET_HOOK(823CB400, XNetKeyRegistration)
TMNT_NET_HOOK(823CB410, XnAddrToInAddr)
TMNT_NET_HOOK(823CB4F0, EthernetLinkStatus)
TMNT_NET_HOOK(823CB450, QosListen)
TMNT_NET_HOOK(823CB470, QosLookup)
TMNT_NET_HOOK(8243D090, Socket)
TMNT_NET_HOOK(8243D0A8, Close)
TMNT_NET_HOOK(8243D0B8, Succeed)
TMNT_NET_HOOK(8243D0D0, Succeed)
TMNT_NET_HOOK(8243D0F0, GetSockName)
TMNT_NET_HOOK(8243D108, Bind)
TMNT_NET_HOOK(8243D120, Connect)
TMNT_NET_HOOK(8243D158, GetOverlappedResult)
TMNT_NET_HOOK(8243D190, RecvFrom)
TMNT_NET_HOOK(8243D1B0, WsaRecvFrom)
TMNT_NET_HOOK(8243D220, SendTo)
TMNT_NET_HOOK(8243D240, WsaSendTo)
TMNT_NET_HOOK(8243D2A0, LastError)
TMNT_NET_HOOK(8243D2C0, WsaWaitForEvents)
TMNT_NET_HOOK(8243D138, Select)
TMNT_NET_HOOK(8243D178, Recv)
TMNT_NET_HOOK(8243D208, Send)
