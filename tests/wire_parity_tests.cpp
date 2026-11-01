// Everything two peers or the online service exchange is byte-for-byte the same on every platform: each encoder must
// reproduce tests/wire-vectors.txt exactly and each decoder must read it back. The same file is checked by the
// online service's tests (server/tmnt-lobby/test/wire.test.js), so Windows, Linux and the Worker agree on one layout.
//
//   tmnt_wire_parity_tests           compare with tests/wire-vectors.txt
//   tmnt_wire_parity_tests --print   print the vectors this build produces
//   tmnt_wire_parity_tests --peer F  compare with the vectors another platform printed into F, and decode them

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "netplay/relay_frame.h"
#include "online/rendezvous.h"
#include "online/stun.h"
#include "online/websocket_frame.h"

#ifndef TMNT_SOURCE_DIR
#error TMNT_SOURCE_DIR must name the repository root
#endif

using namespace tmnt::netplay;
namespace online = tmnt::online;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  PARITY FAIL: %s\n", what.c_str());
  }
}

using Bytes = std::vector<uint8_t>;

std::string ToHex(const Bytes& bytes) {
  std::string out;
  char two[3];
  for (const uint8_t b : bytes) {
    std::snprintf(two, sizeof two, "%02x", b);
    out += two;
  }
  return out;
}

Bytes FromHex(const std::string& text) {
  Bytes out;
  for (size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
  return out;
}

Bytes BigEndian(uint64_t value, int bytes) {
  Bytes out;
  for (int i = bytes - 1; i >= 0; --i) out.push_back(uint8_t(value >> (i * 8)));
  return out;
}

// The sender side of every vector: what this build puts on the wire.
std::map<std::string, Bytes> Produce() {
  std::map<std::string, Bytes> v;
  const Bytes payload = {'l', 'o', 'b', 'b', 'y'};
  v["relay.data"] = EncodeRelayFrame(1, 2, 0x89ABCDEF, 0x00000007, payload.data(), payload.size());
  Bytes game(24);
  for (size_t i = 0; i < game.size(); ++i) game[i] = uint8_t(i);
  v["relay.game"] = EncodeRelayFrame(1, 4, 0x00000001, 0xFFFFFFFF, game.data(), game.size());
  const Bytes slots = {'s', 'l', 'o', 't', '2', 't', 'o', '3'};
  v["relay.v2.slot2to3"] = EncodeRelayFrame(2, 3, 0x12345678, 0x42, slots.data(), slots.size());

  v["punch.punch.host"] = online::EncodePunch({online::PunchKind::kPunch, 1, 0x0123456789ABCDEFull});
  v["punch.ack.join"] = online::EncodePunch({online::PunchKind::kAck, 2, 0xFEDCBA9876543210ull});
  v["punch.key.match"] = BigEndian(online::PunchKey("m-7f3a9c"), 8);

  const online::net::stun::TransactionId id = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
  uint8_t request[online::net::stun::kRequestSize];
  online::net::stun::MakeRequest(id, request);
  v["stun.binding_request"] = Bytes(request, request + sizeof request);
  return v;
}

std::map<std::string, Bytes> ReadVectors(const std::string& path) {
  std::map<std::string, Bytes> v;
  std::ifstream in(path);
  for (std::string line; std::getline(in, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string name, hex;
    fields >> name >> hex;
    v[name] = FromHex(hex);
  }
  return v;
}

// Decoders read the recorded bytes, so a peer on another platform is understood whatever this one encodes.
void TestReceiversReadTheRecordedBytes(const std::map<std::string, Bytes>& recorded) {
  const auto get = [&](const char* name) {
    const auto it = recorded.find(name);
    return it == recorded.end() ? Bytes() : it->second;
  };
  {
    const Bytes b = get("relay.data");
    const auto frame = DecodeRelayFrame(b.data(), b.size());
    Check(frame && frame->src == 1 && frame->dst == 2 && frame->token == 0x89ABCDEF && frame->seq == 7 && frame->payload == Bytes{'l', 'o', 'b', 'b', 'y'},
          "relay binary frame decodes differently from the recorded bytes");
    Check(b.size() == kRelayHeaderBytes + 5 && b[0] == 'S' && b[1] == 'R' && b[2] == 2 && b[14] == 0 && b[15] == 5,
          "relay frame header is not \"SR\", version, type, src, dst, token, seq, big-endian u16 length");
  }
  {
    const Bytes b = get("relay.game");
    const auto frame = DecodeRelayFrame(b.data(), b.size());
    Check(frame && frame->src == 1 && frame->dst == 4 && frame->token == 1 && frame->seq == 0xFFFFFFFF && frame->payload.size() == 24 && frame->payload[23] == 23,
          "relay frame with the largest sequence number decodes differently from the recorded bytes");
  }
  {
    const Bytes b = get("relay.v2.slot2to3");
    const auto frame = DecodeRelayFrame(b.data(), b.size());
    Check(frame && frame->src == 2 && frame->dst == 3 && frame->token == 0x12345678 && frame->seq == 0x42 &&
              frame->payload == Bytes{'s', 'l', 'o', 't', '2', 't', 'o', '3'},
          "relay frame between two guests decodes differently from the recorded bytes");
  }
  {
    const Bytes b = get("punch.ack.join");
    const auto punch = online::DecodePunch(b.data(), b.size());
    Check(punch && punch->kind == online::PunchKind::kAck && punch->side == 2 && punch->key == 0xFEDCBA9876543210ull,
          "UDP punch acknowledgement decodes differently from the recorded bytes");
  }
}

// Ground truth that does not come from this code base.
void TestPublishedVectors(const std::map<std::string, Bytes>& produced) {
  // RFC 5769 2.2: the sample IPv4 Binding success response maps 192.0.2.1:32853.
  const Bytes response = FromHex(
      "0101003c2112a442b7e7a701bc34d686fa87dfae8022000b7465737420766563746f7220002000080001a147e112a643"
      "000800142b91f599fd9e90c38c7489f92af9ba53f06be7d780280004c07d4c96");
  const online::net::stun::TransactionId id = {0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34, 0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae};
  online::net::Endpoint mapped;
  Check(online::net::stun::ParseResponse(response.data(), response.size(), id, mapped) && mapped.ip == 0xC0000201 &&
            mapped.port == 32853,
        "STUN XOR-MAPPED-ADDRESS of the RFC 5769 sample response is not 192.0.2.1:32853");
  const Bytes request = produced.at("stun.binding_request");
  Check(request.size() == 20 && request[0] == 0x00 && request[1] == 0x01 && request[4] == 0x21 && request[5] == 0x12 &&
            request[6] == 0xA4 && request[7] == 0x42 && std::memcmp(request.data() + 8, id.data(), 12) == 0,
        "STUN Binding request is not type 0x0001 with the RFC 5389 magic cookie and the transaction id");
}

// RFC 6455 1.3 and 5.7: the sockets client's handshake answer and frames (the relay rides in binary frames).
void TestWebSocketFraming() {
  namespace ws = online::ws;
  Check(ws::AcceptFor("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
        "WebSocket Sec-WebSocket-Accept is not the RFC 6455 value");
  const Bytes abc = {'a', 'b', 'c'};
  const auto digest = ws::Sha1(abc.data(), abc.size());
  Check(ToHex(Bytes(digest.begin(), digest.end())) == "a9993e364706816aba3e25717850c26c9cd0d89d",
        "SHA-1 of \"abc\" is not the published value");
  for (const auto& [text, base64] : {std::pair{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foobar", "Zm9vYmFy"}}) {
    const std::string in = text;
    Check(ws::Base64(reinterpret_cast<const uint8_t*>(in.data()), in.size()) == base64,
          std::string("Base64 of \"") + text + "\" is not the RFC 4648 value");
  }
  const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  const Bytes hello = {'H', 'e', 'l', 'l', 'o'};
  Check(ws::ClientFrame(1, hello.data(), hello.size(), mask) == FromHex("818537fa213d7f9f4d5158"),
        "masked WebSocket text frame is not the RFC 6455 example");
  const Bytes relay = FromHex("5352010189abcdef0000000700056c6f626279");
  const Bytes framed = ws::ClientFrame(2, relay.data(), relay.size(), mask);
  Check(framed.size() == 2 + 4 + relay.size() && framed[0] == 0x82 && framed[1] == (0x80 | relay.size()),
        "relay frame is not sent as one masked final binary WebSocket frame");
  Bytes unmasked(framed.begin() + 6, framed.end());
  for (size_t i = 0; i < unmasked.size(); ++i) unmasked[i] ^= mask[i & 3];
  Check(unmasked == relay, "masked binary WebSocket frame does not carry the relay bytes");
  const Bytes medium(256, 0x5A);
  const Bytes m = ws::ClientFrame(2, medium.data(), medium.size(), mask);
  Check(m.size() == 4 + 4 + 256 && m[1] == 0xFE && m[2] == 0x01 && m[3] == 0x00,
        "256-byte WebSocket frame does not use the 16-bit length");
  const Bytes large(65536, 0x5A);
  const Bytes l = ws::ClientFrame(2, large.data(), large.size(), mask);
  Check(l.size() == 10 + 4 + 65536 && l[1] == 0xFF && l[7] == 0x01 && l[8] == 0x00 && l[9] == 0x00,
        "64 KiB WebSocket frame does not use the 64-bit length");
}

}  // namespace

int main(int argc, char** argv) {
  const std::map<std::string, Bytes> produced = Produce();
  if (argc > 1 && std::strcmp(argv[1], "--print") == 0) {
    for (const auto& [name, bytes] : produced) std::printf("%s %s\n", name.c_str(), ToHex(bytes).c_str());
    return 0;
  }
  const bool peer = argc > 2 && std::strcmp(argv[1], "--peer") == 0;
  const std::string source = peer ? argv[2] : std::string(TMNT_SOURCE_DIR) + "/tests/wire-vectors.txt";
  const std::map<std::string, Bytes> recorded = ReadVectors(source);
  Check(!recorded.empty(), source + " is missing or empty");
  for (const auto& [name, bytes] : produced) {
    const auto it = recorded.find(name);
    Check(it != recorded.end(), name + " is produced but not recorded in " + source);
    if (it == recorded.end()) continue;
    Check(!bytes.empty(), name + " encodes to nothing");
    Check(it->second == bytes, name + " encodes to " + ToHex(bytes) + ", not the recorded " + ToHex(it->second));
  }
  for (const auto& [name, bytes] : recorded) {
    Check(produced.contains(name), name + " is recorded in " + source + " but no longer produced");
  }
  TestReceiversReadTheRecordedBytes(recorded);
  TestPublishedVectors(produced);
  TestWebSocketFraming();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
