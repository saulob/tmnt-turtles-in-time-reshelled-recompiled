#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tmnt::classic {

// The one .sfc or .smc entry of a ZIP held in memory, refusing anything larger than max_rom_bytes.
bool ExtractRomFromZip(std::span<const std::uint8_t> zip, size_t max_rom_bytes, std::vector<std::uint8_t>& rom,
                       std::string& error);

bool IsZipPath(const std::string& extension);

}  // namespace tmnt::classic
