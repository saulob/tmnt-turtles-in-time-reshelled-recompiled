#include "online/telemetry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <random>

namespace tmnt::online {

namespace {

constexpr size_t kMaxValueChars = 48;
constexpr size_t kMaxScannedInput = 4096;
constexpr std::string_view kRedacted = "redacted";

constexpr std::array<std::string_view, 16> kEventNames = {
    "invite_received",   "invite_failed",      "join_started",       "join_completed",
    "join_failed",       "game_start_attempt", "game_start_success", "game_start_failure",
    "worker_reconnect",  "direct_attempt",     "direct_result",      "relay_fallback",
    "relay_result",      "transport_selected", "peer_disconnect",    "session_end"};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlnum(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool IsUpperAlnum(char c) { return IsDigit(c) || (c >= 'A' && c <= 'Z'); }
bool IsHex(char c) { return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool IsAllowed(char c) { return IsAlnum(c) || c == '_' || c == '.' || c == ':' || c == '-'; }

bool HasFriendCode(std::string_view s) {
  for (size_t i = 0; i + 9 <= s.size(); ++i) {
    if (s[i + 4] != '-') continue;
    bool ok = true;
    for (size_t k = 0; k < 4 && ok; ++k) ok = IsUpperAlnum(s[i + k]) && IsUpperAlnum(s[i + 5 + k]);
    if (ok) return true;
  }
  return false;
}

// Dashes do not break the run, so a dashed UUID is caught too.
bool HasLongHex(std::string_view s) {
  size_t run = 0;
  for (char c : s) {
    if (IsHex(c)) {
      if (++run >= 32) return true;
    } else if (c != '-') {
      run = 0;
    }
  }
  return false;
}

bool HasIpv4(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    if (!IsDigit(s[i]) && s[i] != '.') {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < s.size() && (IsDigit(s[j]) || s[j] == '.')) ++j;
    size_t groups = 0, digits = 0;
    for (size_t k = i; k <= j; ++k) {
      if (k == j || s[k] == '.') {
        groups = (digits >= 1 && digits <= 3) ? groups + 1 : 0;
        digits = 0;
        if (groups >= 4) return true;
      } else {
        ++digits;
      }
    }
    i = j;
  }
  return false;
}

bool HasIpv6(std::string_view s) {
  size_t i = 0;
  while (i < s.size()) {
    if (!IsHex(s[i]) && s[i] != ':' && s[i] != '.') {
      ++i;
      continue;
    }
    size_t j = i, colons = 0;
    while (j < s.size() && (IsHex(s[j]) || s[j] == ':' || s[j] == '.')) colons += s[j++] == ':';
    if (colons >= 2) return true;
    i = j;
  }
  return false;
}

bool HasUrl(std::string_view s) {
  std::string lower(s);
  for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  return lower.find("://") != std::string::npos || lower.find("www.") != std::string::npos ||
         lower.find("mailto:") != std::string::npos;
}

bool HasForbidden(std::string_view s) {
  return HasFriendCode(s) || HasLongHex(s) || HasIpv4(s) || HasIpv6(s) || HasUrl(s);
}

// Not JSON-escaped anywhere below: every string that reaches the output went through SanitizeTelemetryValue.
std::string Quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  out += s;
  out += '"';
  return out;
}

std::string Fixed(double value, int decimals) {
  if (!std::isfinite(value)) return "0";
  value = std::clamp(value, -1e12, 1e12);
  int64_t scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  const int64_t ticks = std::llround(std::fabs(value) * static_cast<double>(scale));
  std::string out = (value < 0 && ticks > 0) ? "-" : "";
  out += std::to_string(ticks / scale);
  if (decimals > 0) {
    std::string frac = std::to_string(ticks % scale);
    out += '.';
    out.append(static_cast<size_t>(decimals) - frac.size(), '0');
    out += frac;
  }
  return out;
}

const char* Bool(bool value) { return value ? "true" : "false"; }

bool ValidSlot(int slot) { return slot >= 1 && slot <= 4; }

std::string ContextJson(const TelemetryContext& c) {
  std::string out = "{\"game\":" + Quote(c.game_id) + ",\"proto\":" + std::to_string(c.protocol) +
                    ",\"version\":" + Quote(c.port_version) + ",\"platform\":" + Quote(c.platform) +
                    ",\"os\":" + Quote(c.os) + ",\"arch\":" + Quote(c.arch) + ",\"fp\":" + Quote(c.fingerprint) +
                    ",\"slot\":" + std::to_string(c.slot) + ",\"host\":" + Bool(c.host) +
                    ",\"party\":" + std::to_string(c.party_size) + "}";
  return out;
}

std::string SessionJson(const SessionReport& s) {
  return "{\"dur_ms\":" + std::to_string(s.duration_ms) + ",\"ct_usable\":" + Bool(s.connection_test_usable) +
         ",\"worker\":" + Bool(s.worker_reachable) + ",\"stun\":" + Bool(s.stun_ok) +
         ",\"direct\":" + Bool(s.direct_available) + ",\"relay\":" + Bool(s.relay_available) +
         ",\"compat\":" + Quote(s.compatibility) + ",\"reason\":" + Quote(s.disconnect_reason) + "}";
}

std::string PeerJson(const PeerReport& p) {
  const PingStats& g = p.ping;
  return "{\"ls\":" + std::to_string(p.local_slot) + ",\"rs\":" + std::to_string(p.remote_slot) +
         ",\"state\":" + Quote(p.state) + ",\"transport\":" + Quote(p.transport) +
         ",\"direct\":" + Quote(p.direct_result) + ",\"relay\":" + Quote(p.relay_result) +
         ",\"fallback\":" + Quote(p.fallback_reason) + ",\"conn_ms\":" + std::to_string(p.connected_ms) +
         ",\"est_ms\":" + std::to_string(p.establishment_ms) + ",\"ping\":{\"sent\":" + std::to_string(g.sent) +
         ",\"recv\":" + std::to_string(g.received) + ",\"last\":" + std::to_string(g.last_ms) +
         ",\"min\":" + std::to_string(g.min_ms) + ",\"max\":" + std::to_string(g.max_ms) +
         ",\"avg\":" + Fixed(g.avg_ms, 1) + ",\"jit\":" + Fixed(g.jitter_ms, 1) +
         ",\"to\":" + std::to_string(g.timeouts) + ",\"ooo\":" + std::to_string(g.out_of_order) +
         ",\"dup\":" + std::to_string(g.duplicates) + ",\"loss\":" + Fixed(g.loss(), 3) +
         "},\"tx\":" + std::to_string(p.sent_packets) + ",\"rx\":" + std::to_string(p.received_packets) +
         ",\"reconn\":" + std::to_string(p.reconnects) + ",\"reason\":" + Quote(p.disconnect_reason) + "}";
}

}  // namespace

std::string SanitizeTelemetryValue(std::string_view value) {
  value = value.substr(0, kMaxScannedInput);
  for (char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '/' || c == '\\' || c == '@' || u <= ' ' || u == 0x7F) return std::string(kRedacted);
  }
  if (HasForbidden(value)) return std::string(kRedacted);

  std::string kept;
  for (char c : value) {
    if (IsAllowed(c)) kept += c;
  }
  // Dropping characters can join pieces into a pattern ("AB!CD-EFGH").
  if (HasForbidden(kept)) return std::string(kRedacted);
  if (kept.size() > kMaxValueChars) kept.resize(kMaxValueChars);
  return kept;
}

std::string_view NormalizeReason(std::string_view value) {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.code == value) return entry.code;
  }
  return reason::other;
}

std::string_view ReasonCategory(std::string_view code) {
  for (const ReasonEntry& entry : kReasons) {
    if (entry.code == code) return entry.category;
  }
  return {};
}

std::string Telemetry::NormalizedReasonField(std::string_view value) {
  const std::string clean = SanitizeTelemetryValue(value);
  if (clean.empty()) return clean;
  const std::string_view known = NormalizeReason(clean);
  if (known == reason::other && clean != reason::other && unknown_reasons_ != std::numeric_limits<uint32_t>::max()) {
    ++unknown_reasons_;
  }
  return std::string(known);
}

std::string Telemetry::NormalizedResultField(std::string_view value) {
  constexpr std::string_view kFailed = "failed:";
  const std::string clean = SanitizeTelemetryValue(value);
  if (clean.rfind(kFailed, 0) != 0) return clean;
  const std::string code = NormalizedReasonField(std::string_view(clean).substr(kFailed.size()));
  return code.empty() ? clean : std::string(kFailed) + code;
}

std::string Telemetry::NewDiagId() {
  uint32_t a = 0, b = 0;
  try {
    std::random_device rd;
    a = rd();
    b = rd();
  } catch (...) {
    a = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    b = a * 2654435761u + 0x9E3779B9u;
  }
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out(12, '0');
  for (size_t i = 0; i < 8; ++i) out[i] = kDigits[(a >> (28 - 4 * i)) & 0xF];
  for (size_t i = 0; i < 4; ++i) out[8 + i] = kDigits[(b >> (12 - 4 * i)) & 0xF];
  return out;
}

void Telemetry::SetEpoch(double now) {
  if (have_epoch_ || !std::isfinite(now)) return;
  have_epoch_ = true;
  epoch_ = now;
}

void Telemetry::CountDropped(uint32_t n) {
  const uint32_t room = std::numeric_limits<uint32_t>::max();
  dropped_total_ = n > room - dropped_total_ ? room : dropped_total_ + n;
  dropped_unreported_ = n > room - dropped_unreported_ ? room : dropped_unreported_ + n;
}

void Telemetry::TrimQueue() {
  uint32_t lost = 0;
  while (events_.size() > kMaxQueued) {
    events_.pop_front();
    ++lost;
  }
  if (lost) CountDropped(lost);
}

void Telemetry::Push(Stored event) {
  events_.push_back(std::move(event));
  TrimQueue();
}

void Telemetry::SetContext(const TelemetryContext& context, double now) {
  try {
    SetEpoch(now);
    TelemetryContext c;
    c.game_id = SanitizeTelemetryValue(context.game_id);
    c.diag = SanitizeTelemetryValue(context.diag);
    c.port_version = SanitizeTelemetryValue(context.port_version);
    c.platform = SanitizeTelemetryValue(context.platform);
    c.os = SanitizeTelemetryValue(context.os);
    c.arch = SanitizeTelemetryValue(context.arch);
    // A full binary hash is 32+ hex and would be redacted as a token; a prefix still tells builds apart.
    c.fingerprint = SanitizeTelemetryValue(std::string_view(context.fingerprint).substr(0, 16));
    c.protocol = context.protocol;
    c.slot = context.slot;
    c.host = context.host;
    c.party_size = context.party_size;
    context_ = std::move(c);
    have_context_ = true;
  } catch (...) {
  }
}

void Telemetry::Event(std::string_view name, std::initializer_list<Field> fields, double now) {
  EventImpl(name, fields.begin(), fields.size(), now);
}

void Telemetry::Event(std::string_view name, const std::vector<Field>& fields, double now) {
  EventImpl(name, fields.data(), fields.size(), now);
}

void Telemetry::EventImpl(std::string_view name, const Field* fields, size_t count, double now) {
  try {
    if (std::find(kEventNames.begin(), kEventNames.end(), name) == kEventNames.end()) {
      if (rejected_ != std::numeric_limits<uint32_t>::max()) ++rejected_;
      return;
    }
    if (!std::isfinite(now)) now = 0;
    SetEpoch(now);

    Stored stored;
    stored.json = "{\"e\":" + Quote(name) + ",\"n\":" + std::to_string(++next_event_) + ",\"t\":" +
                  Fixed(std::max(0.0, now - epoch_), 2);
    size_t written = 0;
    std::string body;
    for (size_t i = 0; i < count; ++i) {
      const auto& [key, value] = fields[i];
      if (written >= kMaxFields) break;
      const std::string k = SanitizeTelemetryValue(key);
      if (k.empty()) continue;
      if (written++) body += ',';
      body += Quote(k) + ":" + Quote(k == "reason" ? NormalizedReasonField(value) : SanitizeTelemetryValue(value));
    }
    if (written) stored.json += ",\"f\":{" + body + "}";
    stored.json += '}';
    Push(std::move(stored));
  } catch (...) {
  }
}

void Telemetry::SetSession(const SessionReport& session) {
  try {
    SessionReport s;
    s.duration_ms = session.duration_ms;
    s.connection_test_usable = session.connection_test_usable;
    s.worker_reachable = session.worker_reachable;
    s.stun_ok = session.stun_ok;
    s.direct_available = session.direct_available;
    s.relay_available = session.relay_available;
    s.compatibility = SanitizeTelemetryValue(session.compatibility);
    s.disconnect_reason = NormalizedReasonField(session.disconnect_reason);
    session_ = std::move(s);
    dirty_ = true;
  } catch (...) {
  }
}

void Telemetry::ClearReports() {
  session_.reset();
  peers_.clear();
  dirty_ = false;
}

void Telemetry::SetPeer(const PeerReport& peer) {
  try {
    if (!ValidSlot(peer.local_slot) || !ValidSlot(peer.remote_slot)) return;
    const auto key = std::make_pair(peer.local_slot, peer.remote_slot);
    if (!peers_.count(key) && peers_.size() >= kMaxPeers) return;
    PeerReport p = peer;
    p.state = SanitizeTelemetryValue(peer.state);
    p.transport = SanitizeTelemetryValue(peer.transport);
    p.direct_result = NormalizedResultField(peer.direct_result);
    p.relay_result = NormalizedResultField(peer.relay_result);
    p.fallback_reason = NormalizedReasonField(peer.fallback_reason);
    p.disconnect_reason = NormalizedReasonField(peer.disconnect_reason);
    peers_[key] = std::move(p);
    dirty_ = true;
  } catch (...) {
  }
}

std::optional<std::string> Telemetry::TakeBatch(double now, bool force) {
  try {
    if (!std::isfinite(now)) now = 0;
    SetEpoch(now);
    if (events_.empty() && !dirty_ && dropped_unreported_ == 0) return std::nullopt;
    if (!force && has_last_batch_ && now >= last_batch_ && now - last_batch_ < kMinSecondsBetweenBatches) {
      return std::nullopt;
    }

    const std::string head = "{\"op\":\"telemetry.batch\",\"diag\":" + Quote(context_.diag) +
                             ",\"ctx\":" + ContextJson(context_);
    const std::string session = session_ ? ",\"session\":" + SessionJson(*session_) : std::string();
    std::vector<std::string> peers;
    for (const auto& [key, report] : peers_) peers.push_back(PeerJson(report));

    // `,"peers":[` + `]`, `,"events":[` + `]`, `,"dropped":` + up to 10 digits, `}`.
    constexpr size_t kWrappers = 11 + 1 + 11 + 1 + 11 + 10 + 1;
    auto peers_size = [&](size_t count) {
      size_t n = 0;
      for (size_t i = 0; i < count; ++i) n += peers[i].size() + 1;
      return n;
    };
    size_t peer_count = peers.size();
    const size_t fixed = head.size() + session.size() + kWrappers;
    // Reports are bounded far below the batch size; if they ever were not, the newest peers give way before events.
    while (peer_count > 0 && fixed + peers_size(peer_count) > kMaxBatchBytes) --peer_count;

    size_t events_size = 0;
    for (const Stored& e : events_) events_size += e.json.size() + 1;
    uint32_t dropped = dropped_unreported_;
    uint32_t cut = 0;
    while (!events_.empty() && fixed + peers_size(peer_count) + events_size > kMaxBatchBytes) {
      events_size -= events_.front().json.size() + 1;
      events_.pop_front();
      ++cut;
    }
    if (cut) {
      CountDropped(cut);
      dropped += cut;
    }

    std::string out = head + session;
    if (peer_count) {
      out += ",\"peers\":[";
      for (size_t i = 0; i < peer_count; ++i) out += (i ? "," : "") + peers[i];
      out += ']';
    }
    out += ",\"events\":[";
    bool first = true;
    for (const Stored& e : events_) {
      if (!first) out += ',';
      first = false;
      out += e.json;
    }
    out += ']';
    if (dropped) out += ",\"dropped\":" + std::to_string(dropped);
    out += '}';

    last_events_.assign(events_.begin(), events_.end());
    last_text_ = out;
    last_dropped_ = dropped;
    events_.clear();
    dropped_unreported_ = 0;
    dirty_ = false;
    has_last_batch_ = true;
    last_batch_ = now;
    return out;
  } catch (...) {
    return std::nullopt;
  }
}

void Telemetry::RestoreBatch(const std::string& batch) {
  try {
    if (last_text_.empty() || batch != last_text_) return;
    std::vector<Stored> events = std::move(last_events_);
    const uint32_t unreported = last_dropped_;
    last_text_.clear();
    last_events_.clear();
    last_dropped_ = 0;

    // The batch may have carried the only copy of a changed report.
    if (session_ || !peers_.empty()) dirty_ = true;

    uint32_t lost = 0;
    std::vector<Stored> back;
    for (Stored& e : events) {
      if (e.retried) {
        ++lost;  // second failure: dropped for good
      } else {
        e.retried = true;
        back.push_back(std::move(e));
      }
    }
    events_.insert(events_.begin(), std::make_move_iterator(back.begin()), std::make_move_iterator(back.end()));
    // Its losses were never reported; they ride the next batch. dropped_total_ already counted them.
    dropped_unreported_ = unreported > std::numeric_limits<uint32_t>::max() - dropped_unreported_
                              ? std::numeric_limits<uint32_t>::max()
                              : dropped_unreported_ + unreported;
    if (lost) CountDropped(lost);
    TrimQueue();
  } catch (...) {
  }
}

}  // namespace tmnt::online
