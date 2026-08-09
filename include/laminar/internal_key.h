#pragma once

#include <cassert>
#include <cstdint>
#include <string>

#include "laminar/coding.h"
#include "laminar/slice.h"
#include "laminar/status.h"

namespace laminar {

using SequenceNumber = uint64_t;
inline constexpr SequenceNumber kMaxSequenceNumber = (1ULL << 56U) - 1U;

enum ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
};

[[nodiscard]] inline bool IsValidValueType(ValueType type) { return type <= kTypeValue; }

[[nodiscard]] inline uint64_t PackSequenceAndType(SequenceNumber sequence, ValueType type) {
  assert(sequence <= kMaxSequenceNumber);
  assert(IsValidValueType(type));
  return (sequence << 8U) | static_cast<uint8_t>(type);
}

inline Status AppendInternalKey(std::string* destination, const Slice& user_key,
                                SequenceNumber sequence, ValueType type) {
  if (destination == nullptr) {
    return Status::InvalidArgument("internal-key destination is null");
  }
  if (sequence > kMaxSequenceNumber) {
    return Status::InvalidArgument("sequence number exceeds 56-bit limit");
  }
  if (!IsValidValueType(type)) {
    return Status::InvalidArgument("unknown value type");
  }
  if (!user_key.empty()) {
    destination->append(user_key.data(), user_key.size());
  }
  PutFixed64(destination, PackSequenceAndType(sequence, type));
  return Status::OK();
}

struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence{0};
  ValueType type{kTypeDeletion};
};

inline bool ParseInternalKey(const Slice& internal_key, ParsedInternalKey* result) {
  if (result == nullptr || internal_key.size() < 8) {
    return false;
  }
  const uint64_t packed = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  const auto type = static_cast<ValueType>(packed & 0xffU);
  if (!IsValidValueType(type)) {
    return false;
  }
  result->type = type;
  result->sequence = packed >> 8U;
  result->user_key = Slice(internal_key.data(), internal_key.size() - 8);
  return true;
}

inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return {internal_key.data(), internal_key.size() - 8};
}

inline int InternalKeyCompare(const Slice& lhs, const Slice& rhs) {
  const int user_result = ExtractUserKey(lhs).Compare(ExtractUserKey(rhs));
  if (user_result != 0) {
    return user_result;
  }
  const uint64_t lhs_packed = DecodeFixed64(lhs.data() + lhs.size() - 8);
  const uint64_t rhs_packed = DecodeFixed64(rhs.data() + rhs.size() - 8);
  if (lhs_packed > rhs_packed) {
    return -1;
  }
  return lhs_packed < rhs_packed ? 1 : 0;
}

} // namespace laminar
