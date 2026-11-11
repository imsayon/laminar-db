#include <atomic>
#include <cerrno>
#include <limits>
#include <thread>

#include "laminar/db.h"
#include "storage/file.h"
#include "storage/format.h"
#include "storage_test_support.h"

namespace {
using namespace laminar;
using namespace laminar::storage;
using namespace laminar::test;

void OrderedPages() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto db = DB::Open(path.string()).value();
  const auto zero = db->LatestSequence().value();
  LAMINAR_CHECK_EQ(zero.sequence, 0U);
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(zero, 1, 1)->required_bytes, 0U);
  WriteBatch batch;
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK_EQ(db->LatestSequence().value(), zero);
  const std::string key("\0\xff", 2), value("x\0y", 3);
  LAMINAR_CHECK(batch.Put(key, value).ok());
  LAMINAR_CHECK(batch.Delete(key).ok());
  LAMINAR_CHECK(batch.Put(key, "").ok());
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK(db->Delete("gone").ok());
  const auto all = db->ReadChangesAfter(zero, 10, 1000).value();
  LAMINAR_CHECK_EQ(all.batches.size(), 2U);
  LAMINAR_CHECK_EQ(all.required_bytes, 0U);
  const auto& first = all.batches[0];
  const auto& second = all.batches[1];
  LAMINAR_CHECK_EQ(first.cursor.sequence, 1U);
  LAMINAR_CHECK_EQ(second.cursor.sequence, 2U);
  LAMINAR_CHECK_EQ(first.operations.size(), 3U);
  LAMINAR_CHECK_EQ(first.operations[0].key, key);
  LAMINAR_CHECK_EQ(first.operations[0].value, value);
  LAMINAR_CHECK_EQ(first.operations[1].type, Change::Type::Delete);
  LAMINAR_CHECK_EQ(first.operations[2].value, "");
  const auto small = db->ReadChangesAfter(zero, 1, first.encoded_bytes - 1).value();
  LAMINAR_CHECK(small.batches.empty());
  LAMINAR_CHECK_EQ(small.required_bytes, first.encoded_bytes);
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(zero, 1, first.encoded_bytes)->batches.size(), 1U);
  const size_t total = first.encoded_bytes + second.encoded_bytes;
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(zero, 10, total - 1)->batches.size(), 1U);
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(zero, 10, total)->batches.size(), 2U);
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(first.cursor, 10, 1000)->batches[0].cursor, second.cursor);
  LAMINAR_CHECK(db->ReadChangesAfter(second.cursor, 1, 1)->batches.empty());
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 0, 1)).IsInvalidArgument());
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 1, 0)).IsInvalidArgument());
  auto invalid = zero;
  invalid.db_id[0] ^= 1;
  LAMINAR_CHECK(Error(db->ReadChangesAfter(invalid, 1, 1000)).IsInvalidArgument());
  invalid = zero;
  invalid.sequence = 3;
  LAMINAR_CHECK(Error(db->ReadChangesAfter(invalid, 1, 1000)).IsInvalidArgument());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(Error(db->LatestSequence()).IsIOError());
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 1, 1000)).IsIOError());
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->LatestSequence().value(), second.cursor);
  LAMINAR_CHECK_EQ(db->ReadChangesAfter(zero, 10, 1000)->batches[0].operations[0].value, value);
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(Compact(path.string()).ok());
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->LatestSequence().value(), second.cursor);
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 1, 1000)).IsNotFound());
  LAMINAR_CHECK(db->ReadChangesAfter(second.cursor, 1, 1000)->batches.empty());
  LAMINAR_CHECK(db->Put("new", "value").ok());
  const auto next = db->ReadChangesAfter(second.cursor, 10, 1000).value();
  LAMINAR_CHECK_EQ(next.batches.size(), 1U);
  LAMINAR_CHECK_EQ(next.batches[0].cursor.sequence, 3U);
  LAMINAR_CHECK_EQ(first.operations[0].value, value); // Returned bytes survive close/compaction.
}

void LargeSequencesAndEmptyBaseline() {
  for (const uint64_t floor : {uint64_t{1} << 53, std::numeric_limits<uint64_t>::max() - 1}) {
    const Scratch scratch;
    const auto path = scratch.root / "db";
    FileHeader header;
    header.id[0] = 'x';
    header.floor = floor;
    InstallFixture(path, EncodeFileHeader(header) + EncodeSeal(0));
    auto db = DB::Open(path.string()).value();
    const auto before = db->LatestSequence().value();
    LAMINAR_CHECK_EQ(before.sequence, floor);
    LAMINAR_CHECK(db->Delete("").ok());
    const auto page = db->ReadChangesAfter(before, 1, 1000).value();
    LAMINAR_CHECK_EQ(page.batches[0].cursor.sequence, floor + 1);
    LAMINAR_CHECK(db->Write(WriteBatch{}).ok());
    if (floor == std::numeric_limits<uint64_t>::max() - 1) {
      LAMINAR_CHECK(db->Put("x", "y").IsFull());
    }
    LAMINAR_CHECK(db->Close().ok());
    LAMINAR_CHECK(Compact(path.string()).ok());
    db = DB::Open(path.string()).value();
    const auto end = db->LatestSequence().value();
    LAMINAR_CHECK_EQ(end.sequence, floor + 1);
    LAMINAR_CHECK(Error(db->ReadChangesAfter(before, 1, 1000)).IsNotFound());
    LAMINAR_CHECK(db->ReadChangesAfter(end, 1, 1000)->batches.empty());
  }
}

Fault FailSync(Point point, std::string_view) noexcept {
  return {point == Point::DataSync ? EIO : 0};
}

void SafeRecovery() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto db = DB::Open(path.string()).value();
  const auto zero = db->LatestSequence().value();
  fault_hook = FailSync;
  const auto status = db->Put("uncertain", "complete");
  fault_hook = nullptr;
  LAMINAR_CHECK(status.IsIOError());
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 1, 1000)).IsIOError());
  LAMINAR_CHECK(Error(db->LatestSequence()).IsIOError());
  LAMINAR_CHECK(db->Close().ok());
  fault_hook = FailSync;
  const auto failed_open = DB::Open(path.string());
  fault_hook = nullptr;
  LAMINAR_CHECK(Error(failed_open).IsIOError());
  db = DB::Open(path.string()).value();
  const auto page = db->ReadChangesAfter(zero, 1, 1000).value();
  LAMINAR_CHECK_EQ(page.batches[0].operations[0].value, "complete");
  LAMINAR_CHECK_EQ(page.batches[0].cursor.sequence, 1U);
}

void NoPartialPage() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto db = DB::Open(path.string()).value();
  const auto zero = db->LatestSequence().value();
  LAMINAR_CHECK(db->Put("one", "1").ok());
  LAMINAR_CHECK(db->Put("two", "2").ok());
  auto bytes = Read(path / "data.log");
  bytes.back() ^= 1;
  Write(path / "data.log", bytes);
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 10, 1000)).IsCorruption());
  LAMINAR_CHECK(Error(db->LatestSequence()).IsIOError());
  LAMINAR_CHECK(db->Close().ok());
  bytes.back() ^= 1;
  Write(path / "data.log", bytes);
  db = DB::Open(path.string()).value();
  const auto first = db->ReadChangesAfter(zero, 1, 1000).value();
  LAMINAR_CHECK_EQ(first.batches.size(), 1U);
  fault_hook = [](Point point, std::string_view) noexcept -> Fault {
    return {point == Point::Read ? EIO : 0};
  };
  const auto failed = db->ReadChangesAfter(first.batches[0].cursor, 1, 1000);
  fault_hook = nullptr;
  LAMINAR_CHECK(Error(failed).IsIOError());
}

void ConcurrentPull() {
  const Scratch scratch;
  auto db = DB::Open((scratch.root / "db").string()).value();
  auto cursor = db->LatestSequence().value();
  std::atomic<bool> done{false}, failed{false};
  std::thread writer([&] {
    for (size_t i = 0; i < 32; ++i) {
      if (!db->Put("key", std::to_string(i)).ok()) {
        failed = true;
      }
    }
    done = true;
  });
  uint64_t seen = 0;
  while (!done || seen < 32) {
    const auto page = db->ReadChangesAfter(cursor, 3, 1000);
    if (!page) {
      failed = true;
      break;
    }
    for (const auto& batch : page->batches) {
      if (batch.cursor.sequence != seen + 1 || batch.operations[0].value != std::to_string(seen)) {
        failed = true;
      }
      cursor = batch.cursor;
      ++seen;
    }
    if (done && page->batches.empty()) {
      break;
    }
    std::this_thread::yield();
  }
  writer.join();
  LAMINAR_CHECK(!failed);
  LAMINAR_CHECK_EQ(seen, 32U);
}
} // namespace

int main() {
  return Run({{"ordered bounded owned pages and restart", OrderedPages},
              {"large sequences and all-deleted checkpoint", LargeSequencesAndEmptyBaseline},
              {"uncertain write recovered only after safe frontier", SafeRecovery},
              {"errors return no partial page", NoPartialPage},
              {"concurrent ordered pull", ConcurrentPull}});
}
