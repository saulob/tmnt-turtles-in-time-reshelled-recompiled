#include "player_match.h"

#include <algorithm>
#include <cstring>

#include "guest_memory.h"
#include "player_match_net.h"

namespace tmnt {
namespace {

constexpr uint64_t kOnlineXuidPrefix = 0x0009000000000000ull;
constexpr uint64_t kXuidBits = 0x0000FFFFFFFFFFFFull;
constexpr uint32_t kSessionIdBytes = 8;
constexpr uint32_t kXnAddrOffset = 8;

uint64_t Fnv1a64(std::string_view text) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (const char c : text) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 0x100000001B3ull;
  }
  return hash;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

void Put32(uint8_t* at, uint32_t value) { StoreBE32(at, 0, value); }

}  // namespace

uint64_t VirtualXuid(std::string_view friend_code) {
  const uint64_t bits = Fnv1a64(std::string("tmnt-xuid:") + std::string(friend_code)) & kXuidBits;
  return kOnlineXuidPrefix | (bits ? bits : 1);
}

std::vector<uint8_t> SessionIdBytes(std::string_view session) {
  std::vector<uint8_t> bytes(kSessionIdBytes, 0);
  if (session.size() != kSessionIdBytes * 2) return bytes;
  for (uint32_t i = 0; i < kSessionIdBytes; ++i) {
    const int hi = HexDigit(session[i * 2]);
    const int lo = HexDigit(session[i * 2 + 1]);
    if (hi < 0 || lo < 0) return std::vector<uint8_t>(kSessionIdBytes, 0);
    bytes[i] = uint8_t(hi << 4 | lo);
  }
  return bytes;
}

std::string SessionIdText(const uint8_t* bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  for (uint32_t i = 0; i < kSessionIdBytes; ++i) {
    text += kHex[bytes[i] >> 4];
    text += kHex[bytes[i] & 0xF];
  }
  return text;
}

void WriteSessionInfo(uint8_t* info, std::string_view session) {
  std::memset(info, 0, kSessionInfoBytes);
  const std::vector<uint8_t> id = SessionIdBytes(session);
  std::memcpy(info, id.data(), id.size());
}

uint32_t WriteSearchResults(uint8_t* header, uint32_t header_address, uint32_t capacity, uint32_t max_results,
                            const std::vector<online::PlayerMatchAd>& ads) {
  const uint32_t count = std::min<uint32_t>(max_results, uint32_t(ads.size()));
  if (capacity < kSearchHeaderBytes + count * kSearchResultBytes) return 0;
  Put32(header, count);
  Put32(header + 4, count ? header_address + kSearchHeaderBytes : 0);
  for (uint32_t i = 0; i < count; ++i) {
    uint8_t* result = header + kSearchHeaderBytes + i * kSearchResultBytes;
    std::memset(result, 0, kSearchResultBytes);
    WriteSessionInfo(result, ads[i].session);
    WriteXnAddr(result + kXnAddrOffset, VirtualXuid(ads[i].player));
    const uint32_t slots = uint32_t(std::clamp(ads[i].slots, 2, online::kMaxParty));
    Put32(result + kSessionInfoBytes + 0, slots - 1);  // open public slots
    Put32(result + kSessionInfoBytes + 8, 1);          // filled public slots: the host
  }
  return count;
}

SessionSlots SearchResultSlots(const uint8_t* result) {
  const auto at = [result](uint32_t offset) { return LoadBE32(result, kSessionInfoBytes + offset); };
  return {at(0) + at(8), at(4) + at(12)};
}

void WriteSigninInfo(uint8_t* info, uint64_t xuid, std::string_view name) {
  StoreBE64(info, 0, xuid);
  StoreBE32(info, 8, kSigninInfoOnlineEnabled);
  StoreBE32(info, 12, kSignedInToLive);
  std::memset(info + 24, 0, 16);
  std::memcpy(info + 24, name.data(), std::min<size_t>(name.size(), 15));
}

uint32_t JoinRefusalResult(const std::string& code) {
  constexpr uint32_t kSessionNotFound = 0x80155200;
  constexpr uint32_t kSessionFull = 0x80155202;
  constexpr uint32_t kServiceUnavailable = 0x80151100;
  constexpr uint32_t kFailed = 0x80004005;
  if (code.empty()) return 0;
  if (code == "party_full") return kSessionFull;
  if (code == "not_found" || code == "offline" || code == "host_left" || code == "expired") return kSessionNotFound;
  if (code == "link_lost" || code == "limit" || code == "rate_limited") return kServiceUnavailable;
  return kFailed;
}

bool WriteSearchByIdResult(uint8_t* header, uint32_t header_address, uint32_t capacity, const online::PlayerMatchAd& ad) {
  std::memset(header, 0, std::min(capacity, kSearchByIdBytes));
  return WriteSearchResults(header, header_address, capacity, 1, {ad}) == 1;
}

void WriteInviteInfo(uint8_t* info, uint64_t invitee, uint64_t inviter, std::string_view session) {
  std::memset(info, 0, kInviteInfoBytes);
  StoreBE64(info, 0, invitee);
  StoreBE64(info, 8, inviter);
  Put32(info + 16, kTmntTitleId);
  WriteSessionInfo(info + 20, session);
  WriteXnAddr(info + 20 + kXnAddrOffset, inviter);
  Put32(info + 80, 1);
}

void MakeNetKey(uint64_t entropy, uint8_t* id, uint8_t* key) {
  uint64_t state = entropy;
  const auto next = [&state] {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  const uint64_t a = next(), b = next(), c = next();
  for (uint32_t i = 0; i < kNetKeyIdBytes; ++i) id[i] = uint8_t(a >> (i * 8));
  id[0] &= 0x0F;
  for (uint32_t i = 0; i < 8; ++i) key[i] = uint8_t(b >> (i * 8)), key[8 + i] = uint8_t(c >> (i * 8));
}

void WriteSessionDetails(uint8_t* out, uint8_t* members_out, uint32_t members_address, const SessionDetails& details) {
  std::memset(out, 0, kDetailsBytes);
  const uint32_t used = std::min(uint32_t(details.members.size()), uint32_t(online::kMaxParty));
  const uint32_t filled_private = std::min(used, details.max_private);
  const uint32_t filled_public = used - filled_private;
  Put32(out + 16, details.max_public);
  Put32(out + 20, details.max_private);
  Put32(out + 24, details.max_public > filled_public ? details.max_public - filled_public : 0);
  Put32(out + 28, details.max_private - filled_private);
  Put32(out + 32, used);
  Put32(out + 36, used);
  Put32(out + 40, details.state);
  const std::vector<uint8_t> id = SessionIdBytes(details.session);
  std::memcpy(out + 56, id.data(), id.size());
  std::memset(members_out, 0, kMemberBytes * online::kMaxParty);
  for (uint32_t i = 0; i < used; ++i) {
    uint8_t* member = members_out + i * kMemberBytes;
    StoreBE64(member, 0, details.members[i].xuid);
    Put32(member + 8, details.members[i].user_index);
  }
  Put32(out + 124, used ? members_address : 0);
}

}  // namespace tmnt
