#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace laminar {

inline void PutFixed32(std::string* destination, uint32_t value) {
  char buffer[4];
  buffer[0] = static_cast<char>(value & 0xffU);
  buffer[1] = static_cast<char>((value >> 8U) & 0xffU);
  buffer[2] = static_cast<char>((value >> 16U) & 0xffU);
  buffer[3] = static_cast<char>((value >> 24U) & 0xffU);
  destination->append(buffer, sizeof(buffer));
}

inline void PutFixed64(std::string* destination, uint64_t value) {
  char buffer[8];
  for (char& byte : buffer) {
    byte = static_cast<char>(value & 0xffU);
    value >>= 8U;
  }
  destination->append(buffer, sizeof(buffer));
}

inline char* EncodeVarint32(char* destination, uint32_t value) {
  while (value >= 0x80U) {
    *destination++ = static_cast<char>((value & 0x7fU) | 0x80U);
    value >>= 7U;
  }
  *destination++ = static_cast<char>(value);
  return destination;
}

inline char* EncodeVarint64(char* destination, uint64_t value) {
  while (value >= 0x80U) {
    *destination++ = static_cast<char>((value & 0x7fU) | 0x80U);
    value >>= 7U;
  }
  *destination++ = static_cast<char>(value);
  return destination;
}

inline void PutVarint32(std::string* destination, uint32_t value) {
  char buffer[5];
  destination->append(buffer, static_cast<size_t>(EncodeVarint32(buffer, value) - buffer));
}

inline void PutVarint64(std::string* destination, uint64_t value) {
  char buffer[10];
  destination->append(buffer, static_cast<size_t>(EncodeVarint64(buffer, value) - buffer));
}

inline bool PutLengthPrefixedSlice(std::string* destination, std::string_view value) {
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  PutVarint32(destination, static_cast<uint32_t>(value.size()));
  destination->append(value);
  return true;
}

inline uint32_t DecodeFixed32(const char* pointer) {
  return static_cast<uint32_t>(static_cast<uint8_t>(pointer[0])) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pointer[1])) << 8U) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pointer[2])) << 16U) |
         (static_cast<uint32_t>(static_cast<uint8_t>(pointer[3])) << 24U);
}

inline uint64_t DecodeFixed64(const char* pointer) {
  uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8U) | static_cast<uint8_t>(pointer[index]);
  }
  return value;
}

inline const char* GetVarint32Ptr(const char* pointer, const char* limit, uint32_t* value) {
  uint32_t result = 0;
  for (uint32_t index = 0; index < 5 && pointer < limit; ++index) {
    const uint32_t byte = static_cast<uint8_t>(*pointer++);
    if (index == 4 && (byte & 0xf0U) != 0) {
      return nullptr;
    }
    result |= (byte & 0x7fU) << (7U * index);
    if ((byte & 0x80U) == 0) {
      *value = result;
      return pointer;
    }
  }
  return nullptr;
}

inline const char* GetVarint64Ptr(const char* pointer, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  for (uint32_t index = 0; index < 10 && pointer < limit; ++index) {
    const uint64_t byte = static_cast<uint8_t>(*pointer++);
    if (index == 9 && (byte & 0xfeU) != 0) {
      return nullptr;
    }
    result |= (byte & 0x7fU) << (7U * index);
    if ((byte & 0x80U) == 0) {
      *value = result;
      return pointer;
    }
  }
  return nullptr;
}

} // namespace laminar
