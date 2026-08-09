#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/memtable.h"
#include "laminar/internal_key.h"
#include "test_harness.h"

namespace {
using laminar::MemTable;
using laminar::SequenceNumber;

void PutGetAndBinaryValues() {
  MemTable table;
  const std::string binary_key{"a\0b", 3};
  const std::string binary_value{"x\0y", 3};
  LAMINAR_CHECK(table.Put(binary_key, binary_value, 1).ok());
  LAMINAR_CHECK(table.Put("", "empty-key", 2).ok());

  std::string value;
  bool deleted = false;
  LAMINAR_CHECK(table.Get(binary_key, 1, &value, &deleted));
  LAMINAR_CHECK(!deleted);
  LAMINAR_CHECK_EQ(value, binary_value);
  LAMINAR_CHECK(table.Get("", 2, &value, &deleted));
  LAMINAR_CHECK_EQ(value, "empty-key");
  LAMINAR_CHECK(!table.Get("missing", 2, &value, &deleted));
  LAMINAR_CHECK(!deleted);
}

void SnapshotsAndTombstones() {
  MemTable table;
  LAMINAR_CHECK(table.Put("key", "v1", 1).ok());
  LAMINAR_CHECK(table.Put("key", "v2", 2).ok());
  LAMINAR_CHECK(table.Delete("key", 3).ok());
  LAMINAR_CHECK(table.Put("key", "v4", 4).ok());

  std::string value;
  bool deleted = false;
  LAMINAR_CHECK(!table.Get("key", 0, &value, &deleted));
  LAMINAR_CHECK(table.Get("key", 1, &value, &deleted));
  LAMINAR_CHECK_EQ(value, "v1");
  LAMINAR_CHECK(table.Get("key", 2, &value, &deleted));
  LAMINAR_CHECK_EQ(value, "v2");
  LAMINAR_CHECK(table.Get("key", 3, &value, &deleted));
  LAMINAR_CHECK(deleted);
  LAMINAR_CHECK(table.Get("key", 4, &value, &deleted));
  LAMINAR_CHECK(!deleted);
  LAMINAR_CHECK_EQ(value, "v4");
}

void ValidationAndDuplicates() {
  MemTable table;
  LAMINAR_CHECK(table.Put("key", "value", 7).ok());
  LAMINAR_CHECK(table.Put("key", "value", 7).IsAlreadyExists());
  LAMINAR_CHECK(
      table.Put("overflow", "value", laminar::kMaxSequenceNumber + 1U).IsInvalidArgument());
  LAMINAR_CHECK(table.Delete("overflow", laminar::kMaxSequenceNumber + 1U).IsInvalidArgument());
}

void IteratorUsesInternalKeyOrder() {
  MemTable table;
  LAMINAR_CHECK(table.Put("banana", "b1", 1).ok());
  LAMINAR_CHECK(table.Put("apple", "a1", 1).ok());
  LAMINAR_CHECK(table.Put("apple", "a3", 3).ok());
  LAMINAR_CHECK(table.Delete("apple", 2).ok());

  auto iterator = table.NewIterator();
  iterator->SeekToFirst();
  const std::vector<std::pair<std::string, SequenceNumber>> expected{
      {"apple", 3}, {"apple", 2}, {"apple", 1}, {"banana", 1}};
  for (const auto& [key, sequence] : expected) {
    LAMINAR_CHECK(iterator->Valid());
    laminar::ParsedInternalKey parsed;
    LAMINAR_CHECK(laminar::ParseInternalKey(iterator->key(), &parsed));
    LAMINAR_CHECK_EQ(parsed.user_key.ToString(), key);
    LAMINAR_CHECK_EQ(parsed.sequence, sequence);
    iterator->Next();
  }
  LAMINAR_CHECK(!iterator->Valid());
}

void FlushThresholdTracksReservedMemory() {
  MemTable table(16U * 1024U);
  LAMINAR_CHECK(!table.ShouldFlush());
  const size_t initial = table.ApproximateMemoryUsage();
  for (uint64_t index = 0; !table.ShouldFlush() && index < 10'000; ++index) {
    LAMINAR_CHECK(table.Put(std::to_string(index), std::string(40, 'x'), index).ok());
  }
  LAMINAR_CHECK(table.ShouldFlush());
  LAMINAR_CHECK(table.ApproximateMemoryUsage() > initial);
}

void RandomizedModelParity() {
  MemTable table;
  std::map<std::string, std::vector<std::pair<SequenceNumber, std::optional<std::string>>>> model;
  std::mt19937 random(0x1a2b3cU);
  SequenceNumber sequence = 0;

  for (int operation = 0; operation < 5'000; ++operation) {
    const std::string key = "key-" + std::to_string(random() % 73U);
    ++sequence;
    if (random() % 5U == 0) {
      LAMINAR_CHECK(table.Delete(key, sequence).ok());
      model[key].emplace_back(sequence, std::nullopt);
    } else {
      std::string value = "value-" + std::to_string(random());
      LAMINAR_CHECK(table.Put(key, value, sequence).ok());
      model[key].emplace_back(sequence, std::move(value));
    }

    const std::string query_key = "key-" + std::to_string(random() % 73U);
    const SequenceNumber snapshot = random() % (sequence + 1U);
    std::optional<std::string> expected;
    bool expected_found = false;
    bool expected_deleted = false;
    if (const auto found = model.find(query_key); found != model.end()) {
      for (auto iterator = found->second.rbegin(); iterator != found->second.rend(); ++iterator) {
        if (iterator->first <= snapshot) {
          expected_found = true;
          expected_deleted = !iterator->second.has_value();
          expected = iterator->second;
          break;
        }
      }
    }

    std::string actual;
    bool actual_deleted = false;
    LAMINAR_CHECK_EQ(table.Get(query_key, snapshot, &actual, &actual_deleted), expected_found);
    if (expected_found) {
      LAMINAR_CHECK_EQ(actual_deleted, expected_deleted);
      if (!expected_deleted) {
        LAMINAR_CHECK_EQ(actual, *expected);
      }
    }
  }
}

void ConcurrentReadersWithSingleWriter() {
  MemTable table;
  constexpr uint64_t kWrites = 4'000;
  std::atomic<bool> done{false};
  std::vector<std::thread> readers;
  for (int thread = 0; thread < 4; ++thread) {
    readers.emplace_back([&table, &done, thread] {
      std::mt19937 random(static_cast<uint32_t>(thread + 1));
      while (!done.load(std::memory_order_acquire)) {
        const uint64_t index = random() % kWrites;
        std::string value;
        bool deleted = false;
        (void)table.Get("key-" + std::to_string(index), laminar::kMaxSequenceNumber, &value,
                        &deleted);
      }
    });
  }
  for (uint64_t index = 0; index < kWrites; ++index) {
    LAMINAR_CHECK(table.Put("key-" + std::to_string(index), "value", index).ok());
  }
  done.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  for (uint64_t index = 0; index < kWrites; ++index) {
    std::string value;
    bool deleted = false;
    LAMINAR_CHECK(table.Get("key-" + std::to_string(index), kWrites, &value, &deleted));
    LAMINAR_CHECK_EQ(value, "value");
  }
}

} // namespace

int main() {
  return laminar::test::Run({
      {"put/get and binary values", PutGetAndBinaryValues},
      {"snapshots and tombstones", SnapshotsAndTombstones},
      {"validation and duplicates", ValidationAndDuplicates},
      {"iterator internal-key order", IteratorUsesInternalKeyOrder},
      {"flush threshold", FlushThresholdTracksReservedMemory},
      {"randomized model parity", RandomizedModelParity},
      {"single-writer concurrent readers", ConcurrentReadersWithSingleWriter},
  });
}
