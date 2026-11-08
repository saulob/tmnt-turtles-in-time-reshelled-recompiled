#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tmnt::classic {

// FNV-1a 64 over the little-endian buffer size and the first kFingerprintBytes of an XMA input buffer, as in
// tools/xma_music_fingerprints.py.
inline constexpr std::uint32_t kFingerprintBytes = 4096;
std::uint64_t XmaStreamFingerprint(const std::uint8_t* data, std::uint32_t bytes);

// Every song plays with the base pack. Stage packs replace samples 0-2 with one-shot stage sound effects, which
// the songs would otherwise key as instruments.
inline constexpr size_t kMusicPack = 0;

struct MusicStream {
  std::uint64_t fingerprint;
  std::uint32_t bytes;
  std::uint32_t wave_key;
  std::string_view context;
  std::uint8_t song_id;
};

// Re-Shelled music waves (TMNT.bf fdd bundles) and the SNES song that replaces each one.
inline constexpr std::array<MusicStream, 14> kMusicStreams = {{
    {0x6E87758BE3897765ull, 1482752, 0x04401670, "StartMenu", 6},
    {0x9C9C47BFCC7F7C57ull, 2818048, 0x04401665, "01 Big Apple 3 AM", 0},
    {0x23CB9DF46687CC0Eull, 2400256, 0x04401666, "02 Alley Cat Blues", 4},
    {0x3611E80F5C0156F9ull, 3809280, 0x04401668, "03 Sewer Surfin'", 9},
    {0x084028CA00A9CF2Dull, 2557952, 0x04401669, "04 Prehistoric Turtlesaurus", 11},
    {0xD5DEE54ED0C9869Bull, 2996224, 0x0440166A, "05 Skull and Crossbones", 12},
    {0x04A172C95A38D713ull, 2656256, 0x0440166B, "06 Bury My Shell at Wounded Knee", 13},
    {0xE583B05D6D0584EEull, 2848768, 0x0440166C, "07 Neon Night-Riders", 14},
    {0xD333A62B86829023ull, 3850240, 0x0440166D, "08 Starbase", 15},
    {0x2225FD1AC6271717ull, 2576384, 0x0440166E, "09 Technodrome", 23},
    {0x02D7B54B1217E074ull, 2781184, 0x0440166F, "boss", 1},
    {0x45CF0ADE14993E53ull, 61440, 0x04401675, "stage clear", 2},
    {0x798788EB7345CD6Dull, 96256, 0x04401672, "time warp", 26},
    {0x77918BDABE3D90E8ull, 1236992, 0x0440168C, "outro", 22},
}};

const MusicStream* FindMusicStream(std::span<const MusicStream> streams, std::uint64_t fingerprint, std::uint32_t bytes);

}  // namespace tmnt::classic
