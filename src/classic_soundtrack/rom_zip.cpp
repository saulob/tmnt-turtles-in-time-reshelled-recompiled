#include "classic_soundtrack/rom_zip.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "updater/third_party/miniz/miniz.h"

namespace tmnt::classic {
namespace {

std::string Lower(std::string_view text) {
  std::string lower(text);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return lower;
}

bool IsRomName(std::string_view name) {
  const std::string lower = Lower(name);
  return lower.size() > 4 && (lower.ends_with(".sfc") || lower.ends_with(".smc"));
}

class ZipReader {
 public:
  explicit ZipReader(std::span<const std::uint8_t> bytes) {
    open_ = mz_zip_reader_init_mem(&zip_, bytes.data(), bytes.size(), 0) != 0;
  }
  ~ZipReader() {
    if (open_) {
      mz_zip_reader_end(&zip_);
    }
  }
  ZipReader(const ZipReader&) = delete;
  ZipReader& operator=(const ZipReader&) = delete;

  bool open() const { return open_; }
  mz_zip_archive* get() { return &zip_; }

 private:
  mz_zip_archive zip_{};
  bool open_ = false;
};

}  // namespace

bool IsZipPath(const std::string& extension) {
  return Lower(extension) == ".zip";
}

bool ExtractRomFromZip(std::span<const std::uint8_t> zip, size_t max_rom_bytes, std::vector<std::uint8_t>& rom,
                       std::string& error) {
  ZipReader reader(zip);
  if (!reader.open()) {
    error = "the file is not a readable ZIP archive";
    return false;
  }
  const mz_uint count = mz_zip_reader_get_num_files(reader.get());
  int found = -1;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(reader.get(), i, &stat) || stat.m_is_directory || !IsRomName(stat.m_filename)) {
      continue;
    }
    if (found >= 0) {
      error = "the ZIP holds more than one SNES ROM";
      return false;
    }
    if (stat.m_is_encrypted || !stat.m_is_supported) {
      error = "the ROM in the ZIP is encrypted or uses an unsupported compression";
      return false;
    }
    if (stat.m_uncomp_size > max_rom_bytes) {
      error = "the ROM in the ZIP is not a 1 MiB SNES ROM image";
      return false;
    }
    found = int(i);
    rom.resize(size_t(stat.m_uncomp_size));
  }
  if (found < 0) {
    error = "the ZIP holds no .sfc or .smc ROM";
    return false;
  }
  if (!mz_zip_reader_extract_to_mem(reader.get(), mz_uint(found), rom.data(), rom.size(), 0)) {
    rom.clear();
    error = "the ROM in the ZIP could not be extracted";
    return false;
  }
  return true;
}

}  // namespace tmnt::classic
