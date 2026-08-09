#pragma once

#include <cstdint>

#include "laminar/slice.h"

namespace laminar::crc32c {

[[nodiscard]] uint32_t Extend(uint32_t initial_crc, const char* data, size_t size);
[[nodiscard]] inline uint32_t Value(const Slice& data) {
  return Extend(0, data.data(), data.size());
}

// Masking prevents stored checksums from being confused with checksums of data containing them.
[[nodiscard]] uint32_t Mask(uint32_t crc);
[[nodiscard]] uint32_t Unmask(uint32_t masked_crc);

} // namespace laminar::crc32c
