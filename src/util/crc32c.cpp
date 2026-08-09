#include "laminar/crc32c.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace laminar::crc32c {
namespace {

constexpr uint32_t kPolynomial = 0x82f63b78U;

consteval std::array<uint32_t, 256> MakeTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t index = 0; index < table.size(); ++index) {
    uint32_t value = index;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value >> 1U) ^ ((value & 1U) != 0U ? kPolynomial : 0U);
    }
    table[index] = value;
  }
  return table;
}

constexpr auto kTable = MakeTable();
constexpr uint32_t kMaskDelta = 0xa282ead8U;

} // namespace

uint32_t Extend(uint32_t initial_crc, const char* data, size_t size) {
  uint32_t crc = ~initial_crc;
  for (size_t index = 0; index < size; ++index) {
    const uint8_t byte = static_cast<uint8_t>(data[index]);
    crc = kTable[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
  }
  return ~crc;
}

uint32_t Mask(uint32_t crc) { return ((crc >> 15U) | (crc << 17U)) + kMaskDelta; }

uint32_t Unmask(uint32_t masked_crc) {
  const uint32_t rotated = masked_crc - kMaskDelta;
  return (rotated >> 17U) | (rotated << 15U);
}

} // namespace laminar::crc32c
