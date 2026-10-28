// Standalone XEX2 decrypt+decompress tool, built only to produce the raw guest memory
// image find-missing-functions.py scans. Reimplements ReXGlue's XexModule::ReadImage*
// (rex/system/xex_module.cpp) without the runtime/memory/kernel dependencies, using the
// same vendored AES (rijndael-alg-fst) and LZX (libmspack lzxd.c) sources; see
// THIRD-PARTY-NOTICES.txt. Built and run by tools/dump-xex-images.ps1.
//
//   dump-xex-image.exe <in.xex> <out.image.bin>
//
// Writes the decrypted, decompressed image starting at the XEX's load address (so
// IMG[0] == guest memory at load_address) and prints load_address, size and SHA-1.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <vector>

extern "C" {
#include "rijndael-alg-fst.h"
}
#include "TinySHA1.hpp"

extern "C" {
#include "mspack/mspack.h"
#include "mspack/lzx.h"
}

namespace {

uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t be16(const uint8_t* p) { return uint16_t(p[0]) << 8 | p[1]; }

const uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                                0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

// Matches aes_decrypt_buffer() in rex/system/xex_module.cpp exactly: AES-128-CBC, zero IV,
// continuing the chain across the whole call (fresh ivec per call).
void AesCbcDecrypt(const uint8_t* key, const uint8_t* in, size_t in_len, uint8_t* out) {
  uint32_t rk[4 * (MAXNR + 1)];
  uint8_t ivec[16] = {0};
  int Nr = rijndaelKeySetupDec(rk, key, 128);
  for (size_t n = 0; n < in_len; n += 16) {
    rijndaelDecrypt(rk, Nr, in + n, out + n);
    for (int i = 0; i < 16; i++) {
      out[n + i] ^= ivec[i];
      ivec[i] = in[n + i];
    }
  }
}

struct MemFile {
  const uint8_t* read_buf = nullptr;
  size_t read_len = 0;
  size_t read_pos = 0;
  uint8_t* write_buf = nullptr;
  size_t write_len = 0;
  size_t write_pos = 0;
};

int MemRead(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  size_t remaining = f->read_len - f->read_pos;
  size_t n = std::min<size_t>(bytes, remaining);
  std::memcpy(buffer, f->read_buf + f->read_pos, n);
  f->read_pos += n;
  return int(n);
}

int MemWrite(mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  size_t remaining = f->write_len - f->write_pos;
  size_t n = std::min<size_t>(bytes, remaining);
  std::memcpy(f->write_buf + f->write_pos, buffer, n);
  f->write_pos += n;
  return int(n);
}

void* MemAlloc(mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemFree(void* p) { std::free(p); }
void MemCopy(void* src, void* dest, size_t bytes) { std::memcpy(dest, src, bytes); }

int LzxDecompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len,
                  uint32_t window_size) {
  uint32_t window_bits = 0;
  uint32_t w = window_size;
  while (w > 1) {
    w >>= 1;
    window_bits++;
  }
  mspack_system sys{};
  sys.read = MemRead;
  sys.write = MemWrite;
  sys.alloc = MemAlloc;
  sys.free = MemFree;
  sys.copy = MemCopy;
  MemFile in{src, src_len, 0, nullptr, 0, 0};
  MemFile out{nullptr, 0, 0, dst, dst_len, 0};
  lzxd_stream* lzxd = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in),
                                reinterpret_cast<mspack_file*>(&out), int(window_bits), 0, 0x8000,
                                off_t(dst_len), 0);
  if (!lzxd) {
    return 1;
  }
  int result = lzxd_decompress(lzxd, off_t(dst_len));
  lzxd_free(lzxd);
  return result;
}

struct PageDescriptor {
  uint32_t page_count;
};

uint32_t ImageSizeFromPages(const std::vector<PageDescriptor>& pages) {
  uint32_t total = 0;
  for (const auto& p : pages) {
    total += p.page_count * 0x10000;
  }
  return total;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: xexdump <in.xex> <out.image.bin>\n");
    return 2;
  }
  FILE* fp = std::fopen(argv[1], "rb");
  if (!fp) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::fseek(fp, 0, SEEK_END);
  long file_size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> file_bytes(static_cast<size_t>(file_size));
  if (std::fread(file_bytes.data(), 1, file_bytes.size(), fp) != file_bytes.size()) {
    std::fprintf(stderr, "short read\n");
    return 1;
  }
  std::fclose(fp);

  const uint8_t* d = file_bytes.data();
  if (std::memcmp(d, "XEX2", 4) != 0) {
    std::fprintf(stderr, "not an XEX2 file\n");
    return 1;
  }
  uint32_t header_size = be32(d + 8);
  uint32_t security_offset = be32(d + 0x10);
  uint32_t header_count = be32(d + 0x14);

  uint32_t ff_offset = 0;
  for (uint32_t i = 0; i < header_count; i++) {
    const uint8_t* entry = d + 0x18 + i * 8;
    uint32_t key = be32(entry);
    if (key == 0x000003FF) {
      ff_offset = be32(entry + 4);
      break;
    }
  }
  if (!ff_offset) {
    std::fprintf(stderr, "no file format info header\n");
    return 1;
  }
  const uint8_t* ff = d + ff_offset;
  uint16_t encryption_type = be16(ff + 4);
  uint16_t compression_type = be16(ff + 6);

  const uint8_t* sec = d + security_offset;
  const uint8_t* aes_key_cipher = sec + 0x150;
  uint32_t load_address = be32(sec + 0x110);
  uint32_t page_descriptor_count = be32(sec + 0x180);
  std::vector<PageDescriptor> pages(page_descriptor_count);
  for (uint32_t i = 0; i < page_descriptor_count; i++) {
    uint32_t val = be32(sec + 0x184 + i * 24);
    pages[i].page_count = val >> 4;
  }
  uint32_t image_size = ImageSizeFromPages(pages);

  uint8_t session_key[16];
  AesCbcDecrypt(kRetailKey, aes_key_cipher, 16, session_key);

  const uint8_t* body = d + header_size;
  size_t exe_length = file_bytes.size() - header_size;

  std::vector<uint8_t> image(image_size, 0);

  if (compression_type == 0) {  // XEX_COMPRESSION_NONE
    if (image.size() != exe_length) {
      std::fprintf(stderr, "uncompressed size mismatch\n");
      return 1;
    }
    if (encryption_type == 1) {
      AesCbcDecrypt(session_key, body, exe_length, image.data());
    } else {
      std::memcpy(image.data(), body, exe_length);
    }
  } else if (compression_type == 1) {  // XEX_COMPRESSION_BASIC
    uint32_t block_count = (be32(ff) - 8) / 8;
    uint32_t rk[4 * (MAXNR + 1)];
    uint8_t ivec[16] = {0};
    int Nr = rijndaelKeySetupDec(rk, session_key, 128);
    const uint8_t* p = body;
    uint8_t* out = image.data();
    for (uint32_t n = 0; n < block_count; n++) {
      const uint8_t* block = ff + 8 + n * 8;
      uint32_t data_size = be32(block);
      uint32_t zero_size = be32(block + 4);
      if (out + data_size > image.data() + image.size()) {
        std::fprintf(stderr, "basic block overflow\n");
        return 1;
      }
      if (encryption_type == 1) {
        for (uint32_t m = 0; m < data_size; m += 16) {
          rijndaelDecrypt(rk, Nr, p + m, out + m);
          for (int i = 0; i < 16; i++) {
            out[m + i] ^= ivec[i];
            ivec[i] = p[m + i];
          }
        }
      } else {
        std::memcpy(out, p, data_size);
      }
      p += data_size;
      out += data_size + zero_size;
    }
  } else if (compression_type == 2) {  // XEX_COMPRESSION_NORMAL (LZX)
    std::vector<uint8_t> decrypted;
    const uint8_t* input_buffer = body;
    if (encryption_type == 1) {
      decrypted.resize(exe_length);
      AesCbcDecrypt(session_key, body, exe_length, decrypted.data());
      input_buffer = decrypted.data();
    }

    uint32_t window_size = be32(ff + 8);
    uint32_t block_size = be32(ff + 12);
    const uint8_t* block_hash = ff + 16;

    std::vector<uint8_t> compress_buffer(exe_length);
    const uint8_t* p = input_buffer;
    uint8_t* cd = compress_buffer.data();
    while (block_size) {
      const uint8_t* pnext = p + block_size;
      uint32_t next_block_size = be32(p);
      const uint8_t* next_block_hash = p + 4;

      sha1::SHA1 s;
      s.processBytes(p, block_size);
      uint8_t digest_bytes[20];
      s.finalize(digest_bytes);
      if (std::memcmp(digest_bytes, block_hash, 20) != 0) {
        std::fprintf(stderr, "block hash mismatch at offset %zd\n", p - input_buffer);
        return 1;
      }

      p += 24;
      for (;;) {
        uint32_t chunk_size = (uint32_t(p[0]) << 8) | p[1];
        p += 2;
        if (!chunk_size) {
          break;
        }
        std::memcpy(cd, p, chunk_size);
        p += chunk_size;
        cd += chunk_size;
      }

      p = pnext;
      block_size = next_block_size;
      block_hash = next_block_hash;
    }

    size_t compressed_len = size_t(cd - compress_buffer.data());
    int rc = LzxDecompress(compress_buffer.data(), compressed_len, image.data(), image.size(),
                           window_size);
    if (rc != 0) {
      std::fprintf(stderr, "lzxd_decompress failed: %d\n", rc);
      return 1;
    }
  } else {
    std::fprintf(stderr, "unsupported compression type %u\n", compression_type);
    return 1;
  }

  FILE* out_fp = std::fopen(argv[2], "wb");
  if (!out_fp || std::fwrite(image.data(), 1, image.size(), out_fp) != image.size()) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  std::fclose(out_fp);

  sha1::SHA1 whole;
  whole.processBytes(image.data(), image.size());
  uint8_t digest[20];
  whole.finalize(digest);
  std::printf("load_address=%08X size=%zX sha1=", load_address, image.size());
  for (uint8_t b : digest) std::printf("%02X", b);
  std::printf("\n");
  return 0;
}
