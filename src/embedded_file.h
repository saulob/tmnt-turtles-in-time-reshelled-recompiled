// A file compiled into the executable (src/embed_files.cmake).
#pragma once

#include <cstddef>
#include <cstdint>

namespace tmnt {

struct EmbeddedFile {
  const char* name;
  const uint8_t* data;
  size_t size;
};

}  // namespace tmnt
