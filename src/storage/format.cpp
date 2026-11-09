#include "storage/format.h"

#include <algorithm>

#include "laminar/coding.h"
#include "laminar/crc32c.h"

namespace laminar::storage {
namespace {

constexpr std::string_view kBaselineMagic = "LMBL";
constexpr std::string_view kSealMagic = "LMNRSEAL";

std::string_view Magic(Kind kind) {
  return kind == Kind::Baseline ? kBaselineMagic : kMutationMagic;
}

bool ValidFrame(Kind kind, uint64_t sequence, size_t count) {
  if (kind == Kind::Baseline) {
    return sequence == 0 && count == 1;
  }
  return sequence != 0 && count != 0 && count <= kMaxOperations;
}

bool ValidOperation(Tag tag, size_t key, size_t value, Kind kind) {
  return key <= kMaxKey && value <= kMaxValue &&
         (tag == Tag::Put || (tag == Tag::Delete && value == 0 && kind == Kind::Mutation));
}

} // namespace

Status Corrupt(std::string_view message) noexcept {
  try {
    return Status::Corruption(message);
  } catch (...) {
    return Status::FromCode(Status::Code::kCorruption);
  }
}

std::string EncodeFileHeader(const FileHeader& header) {
  std::string bytes(kFileMagic);
  bytes.reserve(kFileHeaderBytes);
  PutFixed32(&bytes, 1);
  PutFixed32(&bytes, kFileHeaderBytes);
  bytes.append(header.id.data(), header.id.size());
  PutFixed64(&bytes, header.floor);
  PutFixed64(&bytes, header.row_count);
  PutFixed64(&bytes, header.row_bytes);
  PutFixed32(&bytes, 0);
  PutFixed32(&bytes, crc32c::Value(bytes));
  return bytes;
}

Result<FileHeader> DecodeFileHeader(std::string_view bytes) {
  if (bytes.size() != kFileHeaderBytes || !bytes.starts_with(kFileMagic) ||
      crc32c::Value(bytes.substr(0, 60)) != DecodeFixed32(bytes.data() + 60)) {
    return std::unexpected(Corrupt("invalid file preamble"));
  }
  if (DecodeFixed32(bytes.data() + 8) != 1 || DecodeFixed32(bytes.data() + 56) != 0) {
    return std::unexpected(Status::NotSupported("file version or flags"));
  }

  FileHeader header;
  std::copy_n(bytes.data() + 16, header.id.size(), header.id.begin());
  header.floor = DecodeFixed64(bytes.data() + 32);
  header.row_count = DecodeFixed64(bytes.data() + 40);
  header.row_bytes = DecodeFixed64(bytes.data() + 48);
  if (DecodeFixed32(bytes.data() + 12) != kFileHeaderBytes ||
      std::ranges::all_of(header.id, [](char byte) { return byte == 0; }) ||
      (header.floor == 0 && header.row_count != 0) ||
      (header.row_count == 0 && header.row_bytes != 0) ||
      header.row_count > header.row_bytes / (kFrameHeaderBytes + kOperationBytes)) {
    return std::unexpected(Corrupt("invalid file header fields"));
  }
  return header;
}

std::string EncodeSeal(uint32_t crc) {
  std::string bytes(kSealMagic);
  PutFixed32(&bytes, crc);
  return bytes;
}

Status ValidateSeal(std::string_view bytes, uint32_t crc) {
  if (bytes.size() != kSealBytes || !bytes.starts_with(kSealMagic) ||
      DecodeFixed32(bytes.data() + 8) != crc) {
    return Corrupt("invalid baseline seal");
  }
  return Status::OK();
}

Result<std::string> EncodeFrame(Kind kind, uint64_t sequence, std::span<const Operation> ops) {
  if (!ValidFrame(kind, sequence, ops.size())) {
    return std::unexpected(Status::InvalidArgument("invalid frame sequence or count"));
  }
  size_t payload_bytes = 0;
  for (const auto& op : ops) {
    if (!ValidOperation(op.tag, op.key.size(), op.value.size(), kind)) {
      return std::unexpected(Status::InvalidArgument("invalid operation or key/value limit"));
    }
    const size_t size = kOperationBytes + op.key.size() + op.value.size();
    if (size > kMaxPayload - payload_bytes) {
      return std::unexpected(Status::InvalidArgument("frame payload limit"));
    }
    payload_bytes += size;
  }

  std::string bytes(kFrameHeaderBytes, '\0');
  bytes.reserve(kFrameHeaderBytes + payload_bytes);
  for (const auto& op : ops) {
    bytes.push_back(static_cast<char>(op.tag));
    PutFixed32(&bytes, static_cast<uint32_t>(op.key.size()));
    PutFixed32(&bytes, static_cast<uint32_t>(op.value.size()));
    bytes.append(op.key);
    bytes.append(op.value);
  }
  std::string header(Magic(kind));
  header.reserve(kFrameHeaderBytes);
  PutFixed32(&header, static_cast<uint32_t>(payload_bytes));
  PutFixed32(&header, static_cast<uint32_t>(ops.size()));
  PutFixed32(&header, 0);
  PutFixed64(&header, sequence);
  PutFixed32(&header, crc32c::Value(Slice(bytes.data() + kFrameHeaderBytes, payload_bytes)));
  PutFixed32(&header, crc32c::Value(header));
  bytes.replace(0, kFrameHeaderBytes, header);
  return bytes;
}

Result<FrameHeader> DecodeFrameHeader(std::string_view bytes, Kind kind) {
  if (bytes.size() != kFrameHeaderBytes || !bytes.starts_with(Magic(kind)) ||
      crc32c::Value(bytes.substr(0, 28)) != DecodeFixed32(bytes.data() + 28)) {
    return std::unexpected(Corrupt("invalid frame header"));
  }
  const FrameHeader header{DecodeFixed32(bytes.data() + 4), DecodeFixed32(bytes.data() + 8),
                           DecodeFixed64(bytes.data() + 16), DecodeFixed32(bytes.data() + 24)};
  if (DecodeFixed32(bytes.data() + 12) != 0 || !ValidFrame(kind, header.sequence, header.count) ||
      header.payload_bytes > kMaxPayload || header.count > header.payload_bytes / kOperationBytes) {
    return std::unexpected(Corrupt("invalid frame fields"));
  }
  return header;
}

Result<std::vector<Operation>> DecodePayload(const FrameHeader& header, Kind kind,
                                             std::string_view bytes) {
  if (bytes.size() != header.payload_bytes || bytes.size() > kMaxPayload ||
      !ValidFrame(kind, header.sequence, header.count) ||
      header.count > bytes.size() / kOperationBytes || crc32c::Value(bytes) != header.payload_crc) {
    return std::unexpected(Corrupt("invalid frame payload"));
  }
  std::vector<Operation> ops;
  ops.reserve(header.count);
  for (uint32_t ordinal = 0; ordinal < header.count; ++ordinal) {
    if (bytes.size() < kOperationBytes) {
      return std::unexpected(Corrupt("short operation header"));
    }
    const auto tag = static_cast<Tag>(bytes[0]);
    const size_t key_bytes = DecodeFixed32(bytes.data() + 1);
    const size_t value_bytes = DecodeFixed32(bytes.data() + 5);
    bytes.remove_prefix(kOperationBytes);
    if (!ValidOperation(tag, key_bytes, value_bytes, kind) || key_bytes > bytes.size() ||
        value_bytes > bytes.size() - key_bytes) {
      return std::unexpected(Corrupt("invalid operation tag or lengths"));
    }
    ops.push_back({tag, bytes.substr(0, key_bytes), bytes.substr(key_bytes, value_bytes)});
    bytes.remove_prefix(key_bytes + value_bytes);
  }
  if (!bytes.empty()) {
    return std::unexpected(Corrupt("excess payload bytes"));
  }
  return ops;
}

} // namespace laminar::storage
