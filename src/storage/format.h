#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "laminar/status.h"

namespace laminar::storage {

inline constexpr std::string_view kFileMagic = "LMNRDATA";
inline constexpr std::string_view kMutationMagic = "LMBT";
inline constexpr size_t kFileHeaderBytes = 64;
inline constexpr size_t kFrameHeaderBytes = 32;
inline constexpr size_t kSealBytes = 12;
inline constexpr size_t kOperationBytes = 9;
inline constexpr size_t kMaxKey = 65'536;
inline constexpr size_t kMaxValue = 8'388'608;
inline constexpr size_t kMaxPayload = 16'777'216;
inline constexpr size_t kMaxOperations = 1'024;

struct FileHeader {
  std::array<char, 16> id{};
  uint64_t floor{};
  uint64_t row_count{};
  uint64_t row_bytes{};
};

enum class Kind : uint8_t { Baseline, Mutation };
enum class Tag : char { Put = 1, Delete = 2 };

struct Operation {
  Tag tag;
  std::string_view key;
  std::string_view value;
};

struct FrameHeader {
  uint32_t payload_bytes;
  uint32_t count;
  uint64_t sequence;
  uint32_t payload_crc;
};

Status Corrupt(std::string_view message) noexcept;
std::string EncodeFileHeader(const FileHeader& header);
Result<FileHeader> DecodeFileHeader(std::string_view bytes);
std::string EncodeSeal(uint32_t crc);
Status ValidateSeal(std::string_view bytes, uint32_t crc);

Result<std::string> EncodeFrame(Kind kind, uint64_t sequence, std::span<const Operation> ops);
Result<FrameHeader> DecodeFrameHeader(std::string_view bytes, Kind kind);
// The returned views borrow the complete, validated payload.
Result<std::vector<Operation>> DecodePayload(const FrameHeader& header, Kind kind,
                                             std::string_view bytes);

} // namespace laminar::storage
