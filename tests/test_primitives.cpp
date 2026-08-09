#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "laminar/arena.h"
#include "laminar/coding.h"
#include "laminar/crc32c.h"
#include "laminar/internal_key.h"
#include "laminar/skip_list.h"
#include "laminar/slice.h"
#include "laminar/status.h"
#include "test_harness.h"

namespace {

void StatusAndResult() {
  LAMINAR_CHECK(laminar::Status::OK().ok());
  const auto error = laminar::Status::Corruption("bad block");
  LAMINAR_CHECK(error.IsCorruption());
  LAMINAR_CHECK_EQ(error.ToString(), "Corruption: bad block");
  const laminar::Result<int> result = 42;
  LAMINAR_CHECK(result.has_value());
  LAMINAR_CHECK_EQ(*result, 42);
}

void SliceIsBinarySafe() {
  const std::string bytes{"a\0b", 3};
  const laminar::Slice slice(bytes);
  LAMINAR_CHECK_EQ(slice.size(), 3U);
  LAMINAR_CHECK_EQ(slice.ToString(), bytes);
  LAMINAR_CHECK(laminar::Slice{} == laminar::Slice{});
  LAMINAR_CHECK(laminar::Slice("a") < laminar::Slice("aa"));
  LAMINAR_CHECK(laminar::Slice("b") > laminar::Slice("aa"));
}

void FixedAndVarintRoundTrips() {
  std::string fixed;
  laminar::PutFixed32(&fixed, 0x78563412U);
  laminar::PutFixed64(&fixed, 0x1020304050607080ULL);
  LAMINAR_CHECK_EQ(laminar::DecodeFixed32(fixed.data()), 0x78563412U);
  LAMINAR_CHECK_EQ(laminar::DecodeFixed64(fixed.data() + 4), 0x1020304050607080ULL);

  const std::vector<uint64_t> values{0,
                                     1,
                                     127,
                                     128,
                                     16'383,
                                     16'384,
                                     std::numeric_limits<uint32_t>::max(),
                                     std::numeric_limits<uint64_t>::max()};
  for (const uint64_t expected : values) {
    std::string encoded;
    laminar::PutVarint64(&encoded, expected);
    uint64_t actual = 0;
    const char* end =
        laminar::GetVarint64Ptr(encoded.data(), encoded.data() + encoded.size(), &actual);
    LAMINAR_CHECK_EQ(end, encoded.data() + encoded.size());
    LAMINAR_CHECK_EQ(actual, expected);
  }
}

void MalformedVarintsAreRejected() {
  const std::string truncated(1, static_cast<char>(0x80));
  uint32_t value32 = 0;
  LAMINAR_CHECK_EQ(
      laminar::GetVarint32Ptr(truncated.data(), truncated.data() + truncated.size(), &value32),
      nullptr);
  const std::string overflow32(5, static_cast<char>(0xff));
  LAMINAR_CHECK_EQ(
      laminar::GetVarint32Ptr(overflow32.data(), overflow32.data() + overflow32.size(), &value32),
      nullptr);
  std::string overflow64(10, static_cast<char>(0x80));
  overflow64.back() = 2;
  uint64_t value64 = 0;
  LAMINAR_CHECK_EQ(
      laminar::GetVarint64Ptr(overflow64.data(), overflow64.data() + overflow64.size(), &value64),
      nullptr);
}

void Crc32cKnownAnswers() {
  const std::string input = "123456789";
  const uint32_t checksum = laminar::crc32c::Value(input);
  LAMINAR_CHECK_EQ(checksum, 0xe3069283U);
  const uint32_t first = laminar::crc32c::Extend(0, input.data(), 4);
  LAMINAR_CHECK_EQ(laminar::crc32c::Extend(first, input.data() + 4, input.size() - 4), checksum);
  LAMINAR_CHECK_EQ(laminar::crc32c::Unmask(laminar::crc32c::Mask(checksum)), checksum);
}

void ArenaAlignmentAndUsage() {
  laminar::Arena arena;
  const char* ordinary = arena.Allocate(3);
  const char* aligned64 = arena.AllocateAligned(17, 64);
  const char* aligned4096 = arena.AllocateAligned(4096, 4096);
  LAMINAR_CHECK_EQ(reinterpret_cast<uintptr_t>(ordinary) % alignof(std::max_align_t), 0U);
  LAMINAR_CHECK_EQ(reinterpret_cast<uintptr_t>(aligned64) % 64U, 0U);
  LAMINAR_CHECK_EQ(reinterpret_cast<uintptr_t>(aligned4096) % 4096U, 0U);
  LAMINAR_CHECK(arena.MemoryUsage() >= 8192U);
}

void InternalKeyValidationAndOrdering() {
  std::string newest;
  std::string oldest;
  std::string deletion;
  LAMINAR_CHECK(laminar::AppendInternalKey(&newest, "key", 3, laminar::kTypeValue).ok());
  LAMINAR_CHECK(laminar::AppendInternalKey(&oldest, "key", 1, laminar::kTypeValue).ok());
  LAMINAR_CHECK(laminar::AppendInternalKey(&deletion, "key", 3, laminar::kTypeDeletion).ok());
  LAMINAR_CHECK(laminar::InternalKeyCompare(newest, deletion) < 0);
  LAMINAR_CHECK(laminar::InternalKeyCompare(deletion, oldest) < 0);
  LAMINAR_CHECK(laminar::AppendInternalKey(&newest, "bad", laminar::kMaxSequenceNumber + 1U,
                                           laminar::kTypeValue)
                    .IsInvalidArgument());

  laminar::ParsedInternalKey parsed;
  LAMINAR_CHECK(laminar::ParseInternalKey(oldest, &parsed));
  LAMINAR_CHECK_EQ(parsed.user_key.ToString(), "key");
  LAMINAR_CHECK_EQ(parsed.sequence, 1U);
  LAMINAR_CHECK(!laminar::ParseInternalKey("tiny", &parsed));
}

void SkipListOrderingAndDuplicates() {
  laminar::Arena arena;
  auto comparator = [](const char* lhs, const char* rhs) { return std::strcmp(lhs, rhs); };
  laminar::SkipList list(comparator, &arena);
  auto copy = [&arena](const char* value) {
    const size_t size = std::strlen(value) + 1U;
    char* destination = arena.Allocate(size);
    std::memcpy(destination, value, size);
    return destination;
  };

  LAMINAR_CHECK(list.Insert(copy("charlie")));
  LAMINAR_CHECK(list.Insert(copy("alpha")));
  LAMINAR_CHECK(list.Insert(copy("bravo")));
  LAMINAR_CHECK(!list.Insert(copy("bravo")));
  LAMINAR_CHECK(list.Contains("alpha"));
  LAMINAR_CHECK(!list.Contains("delta"));

  laminar::SkipList::Iterator iterator(&list);
  iterator.SeekToFirst();
  for (const char* expected : {"alpha", "bravo", "charlie"}) {
    LAMINAR_CHECK(iterator.Valid());
    LAMINAR_CHECK_EQ(std::string(iterator.key()), expected);
    iterator.Next();
  }
  LAMINAR_CHECK(!iterator.Valid());
  iterator.SeekToLast();
  LAMINAR_CHECK_EQ(std::string(iterator.key()), "charlie");
  iterator.Prev();
  LAMINAR_CHECK_EQ(std::string(iterator.key()), "bravo");
}

} // namespace

int main() {
  return laminar::test::Run({
      {"status and result", StatusAndResult},
      {"binary-safe Slice", SliceIsBinarySafe},
      {"fixed and varint round trips", FixedAndVarintRoundTrips},
      {"malformed varints", MalformedVarintsAreRejected},
      {"CRC32C known answers", Crc32cKnownAnswers},
      {"arena alignment", ArenaAlignmentAndUsage},
      {"internal-key validation", InternalKeyValidationAndOrdering},
      {"skip-list ordering", SkipListOrderingAndDuplicates},
  });
}
