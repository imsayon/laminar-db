#include <array>
#include <limits>

#include "laminar/coding.h"
#include "laminar/crc32c.h"
#include "storage/format.h"
#include "storage_test_support.h"
#include "v1_fixtures.h"

namespace {

using namespace laminar;
using namespace laminar::storage;
using namespace laminar::test;

void GoldenBytes() {
  const auto fresh = DecodeFileHeader(kFresh.substr(0, kFileHeaderBytes)).value();
  for (size_t i = 0; i < fresh.id.size(); ++i) {
    LAMINAR_CHECK_EQ(fresh.id[i], static_cast<char>(i));
  }
  LAMINAR_CHECK_EQ(fresh.floor, 0U);
  LAMINAR_CHECK_EQ(EncodeFileHeader(fresh) + EncodeSeal(0), kFresh);

  const std::string_view key("a\0", 2);
  const std::string_view value("\0\xff", 2);
  const Operation put{Tag::Put, key, value};
  const std::array mixed{put, Operation{Tag::Delete, {}, {}}};
  const auto batch = EncodeFrame(Kind::Mutation, 1, mixed).value();
  LAMINAR_CHECK_EQ(std::string(kFresh) + batch, kMixed);
  const auto batch_header =
      DecodeFrameHeader(kMixed.substr(kFresh.size(), kFrameHeaderBytes), Kind::Mutation).value();
  LAMINAR_CHECK_EQ(batch_header.sequence, 1U);
  const auto decoded =
      DecodePayload(batch_header, Kind::Mutation, kMixed.substr(kFresh.size() + kFrameHeaderBytes))
          .value();
  LAMINAR_CHECK_EQ(decoded.size(), 2U);
  LAMINAR_CHECK_EQ(decoded[0].key, key);
  LAMINAR_CHECK_EQ(decoded[0].value, value);
  LAMINAR_CHECK_EQ(decoded[1].tag, Tag::Delete);
  LAMINAR_CHECK(decoded[1].key.empty());

  const auto row = EncodeFrame(Kind::Baseline, 0, std::span(&put, 1)).value();
  const auto row_header =
      DecodeFrameHeader(std::string_view(row).substr(0, kFrameHeaderBytes), Kind::Baseline).value();
  const auto payload = std::string_view(row).substr(kFrameHeaderBytes);
  LAMINAR_CHECK_EQ(DecodePayload(row_header, Kind::Baseline, payload).value()[0].value, value);
  LAMINAR_CHECK_EQ(EncodeFileHeader({fresh.id, 1, 1, row.size()}) + row +
                       EncodeSeal(crc32c::Value(payload)),
                   kCheckpoint);
  LAMINAR_CHECK_EQ(EncodeFileHeader({fresh.id, 2, 0, 0}) + EncodeSeal(0), kAllDeleted);
  for (const auto image : {kFresh, kCheckpoint, kAllDeleted}) {
    const auto seal = image.substr(image.size() - kSealBytes);
    LAMINAR_CHECK(ValidateSeal(seal, DecodeFixed32(seal.data() + 8)).ok());
    LAMINAR_CHECK(ValidateSeal(seal.substr(0, kSealBytes - 1), 0).IsCorruption());
  }
}

void Limits() {
  const std::string key(kMaxKey + 1, 'k');
  const std::string value(kMaxValue + 1, 'v');
  const Operation at_limit{Tag::Put, std::string_view(key).substr(1),
                           std::string_view(value).substr(1)};
  const auto frame =
      EncodeFrame(Kind::Mutation, std::numeric_limits<uint64_t>::max(), std::span(&at_limit, 1))
          .value();
  const auto bytes = std::string_view(frame);
  const auto header = DecodeFrameHeader(bytes.substr(0, kFrameHeaderBytes), Kind::Mutation).value();
  LAMINAR_CHECK_EQ(DecodePayload(header, Kind::Mutation, bytes.substr(kFrameHeaderBytes))
                       .value()[0]
                       .value.size(),
                   kMaxValue);

  for (const auto bad : {Operation{Tag::Put, key, {}}, Operation{Tag::Put, {}, value}}) {
    LAMINAR_CHECK(Error(EncodeFrame(Kind::Mutation, 1, std::span(&bad, 1))).IsInvalidArgument());
  }

  std::vector<Operation> many(kMaxOperations, {Tag::Delete, {}, {}});
  LAMINAR_CHECK(EncodeFrame(Kind::Mutation, 1, many));
  many.push_back({Tag::Delete, {}, {}});
  LAMINAR_CHECK(Error(EncodeFrame(Kind::Mutation, 1, many)).IsInvalidArgument());
  LAMINAR_CHECK(Error(EncodeFrame(Kind::Mutation, 1, {})).IsInvalidArgument());
  LAMINAR_CHECK(Error(EncodeFrame(Kind::Mutation, 0, std::span(&at_limit, 1))).IsInvalidArgument());

  std::array aggregate{Operation{Tag::Put, {}, at_limit.value},
                       Operation{Tag::Put, {}, at_limit.value.substr(2 * kOperationBytes)}};
  const auto max = EncodeFrame(Kind::Mutation, 1, aggregate).value();
  LAMINAR_CHECK_EQ(max.size(), kFrameHeaderBytes + kMaxPayload);
  aggregate[1].value = at_limit.value.substr(2 * kOperationBytes - 1);
  LAMINAR_CHECK(Error(EncodeFrame(Kind::Mutation, 1, aggregate)).IsInvalidArgument());
}

void Set32(std::string& bytes, size_t offset, uint32_t value) {
  std::string encoded;
  PutFixed32(&encoded, value);
  bytes.replace(offset, 4, encoded);
}

void MalformedBytes() {
  const auto file = kFresh.substr(0, kFileHeaderBytes);
  for (size_t i = 0; i < file.size(); ++i) {
    auto bad = std::string(file);
    bad[i] ^= 1;
    LAMINAR_CHECK(Error(DecodeFileHeader(bad)).IsCorruption());
    LAMINAR_CHECK(Error(DecodeFileHeader(file.substr(0, i))).IsCorruption());
  }
  for (const size_t field : {8U, 12U, 32U, 40U, 48U, 56U}) {
    auto bad = std::string(file);
    Set32(bad, field, field == 8 ? 2U : 1U);
    Set32(bad, 60, crc32c::Value(std::string_view(bad).substr(0, 60)));
    const auto decoded = DecodeFileHeader(bad);
    if (field == 32) {
      LAMINAR_CHECK_EQ(decoded.value().floor, 1U);
    } else {
      LAMINAR_CHECK_EQ(Error(decoded).code(), field == 8 || field == 56
                                                  ? Status::Code::kNotSupported
                                                  : Status::Code::kCorruption);
    }
  }
  auto zero_id = std::string(file);
  zero_id.replace(16, 16, 16, '\0');
  Set32(zero_id, 60, crc32c::Value(std::string_view(zero_id).substr(0, 60)));
  LAMINAR_CHECK(Error(DecodeFileHeader(zero_id)).IsCorruption());

  const auto frame = kMixed.substr(kFresh.size(), kFrameHeaderBytes);
  const auto payload = kMixed.substr(kFresh.size() + kFrameHeaderBytes);
  const auto header = DecodeFrameHeader(frame, Kind::Mutation).value();
  for (size_t i = 0; i < frame.size(); ++i) {
    auto bad = std::string(frame);
    bad[i] ^= 1;
    LAMINAR_CHECK(Error(DecodeFrameHeader(bad, Kind::Mutation)).IsCorruption());
    LAMINAR_CHECK(Error(DecodeFrameHeader(frame.substr(0, i), Kind::Mutation)).IsCorruption());
  }
  for (const auto& [field, value] :
       std::array{std::pair{4U, static_cast<uint32_t>(kMaxPayload + 1)}, std::pair{4U, 17U},
                  std::pair{8U, 0U}, std::pair{8U, static_cast<uint32_t>(kMaxOperations + 1)},
                  std::pair{12U, 1U}, std::pair{16U, 0U}}) {
    auto bad = std::string(frame);
    Set32(bad, field, value);
    Set32(bad, 28, crc32c::Value(std::string_view(bad).substr(0, 28)));
    LAMINAR_CHECK(Error(DecodeFrameHeader(bad, Kind::Mutation)).IsCorruption());
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    auto bad = std::string(payload);
    bad[i] ^= 1;
    LAMINAR_CHECK(Error(DecodePayload(header, Kind::Mutation, bad)).IsCorruption());
    LAMINAR_CHECK(
        Error(DecodePayload(header, Kind::Mutation, payload.substr(0, i))).IsCorruption());
  }
  for (const auto& [offset, value] : std::array{std::pair{0U, 3U}, std::pair{1U, 0xffU},
                                                std::pair{5U, 0xffU}, std::pair{18U, 1U}}) {
    auto bad = std::string(payload);
    bad[offset] = static_cast<char>(value);
    auto checked = header;
    checked.payload_crc = crc32c::Value(bad);
    LAMINAR_CHECK(Error(DecodePayload(checked, Kind::Mutation, bad)).IsCorruption());
  }
  auto excess = header;
  excess.count = 1;
  LAMINAR_CHECK(Error(DecodePayload(excess, Kind::Mutation, payload)).IsCorruption());
}

} // namespace

int main() {
  return laminar::test::Run({{"V1 golden bytes", GoldenBytes},
                             {"codec limits", Limits},
                             {"malformed bytes", MalformedBytes}});
}
