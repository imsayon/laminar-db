#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "laminar/coding.h"
#include "laminar/crc32c.h"
#include "storage/format.h"

namespace {

using namespace laminar;
using namespace laminar::storage;

void CheckRoundTrips(std::string_view bytes) {
  if (const auto file = DecodeFileHeader(bytes); file && EncodeFileHeader(*file) != bytes) {
    std::abort();
  }
  const auto prefix = bytes.substr(0, kFrameHeaderBytes);
  for (const auto kind : {Kind::Baseline, Kind::Mutation}) {
    const auto header = DecodeFrameHeader(prefix, kind);
    if (!header) {
      continue;
    }
    const auto ops = DecodePayload(*header, kind, bytes.substr(kFrameHeaderBytes));
    if (ops) {
      const auto round_trip = EncodeFrame(kind, header->sequence, *ops);
      if (!round_trip || *round_trip != bytes) {
        std::abort();
      }
    }
  }
  if (bytes.size() == kSealBytes) {
    const uint32_t crc = DecodeFixed32(bytes.data() + 8);
    if (ValidateSeal(bytes, crc).ok() && EncodeSeal(crc) != bytes) {
      std::abort();
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* input, size_t size) {
  if (size > kFrameHeaderBytes + kMaxPayload) {
    return 0;
  }
  const std::string_view bytes(reinterpret_cast<const char*>(input), size);
  CheckRoundTrips(bytes);

  // Recompute checksums on a second pass to exercise semantic checks after the CRC gates.
  if (size == kFileHeaderBytes && bytes.starts_with(kFileMagic)) {
    std::string checksummed(bytes.substr(0, 60));
    PutFixed32(&checksummed, crc32c::Value(checksummed));
    CheckRoundTrips(checksummed);
  } else if (size >= kFrameHeaderBytes &&
             (bytes.starts_with(kMutationMagic) || bytes.starts_with("LMBL"))) {
    const auto payload = bytes.substr(kFrameHeaderBytes);
    std::string checksummed(bytes.substr(0, 24));
    PutFixed32(&checksummed, crc32c::Value(payload));
    PutFixed32(&checksummed, crc32c::Value(checksummed));
    checksummed.append(payload);
    CheckRoundTrips(checksummed);
  }
  return 0;
}
