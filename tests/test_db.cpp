#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>

#include "laminar/coding.h"
#include "laminar/crc32c.h"
#include "laminar/db.h"
#include "storage/file.h"
#include "storage/format.h"
#include "storage_test_support.h"
#include "v1_fixtures.h"

namespace {

using namespace laminar;
using namespace laminar::storage;
using namespace laminar::test;
namespace fs = std::filesystem;

void SingletonAPI() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  LAMINAR_CHECK(Error(DB::Open("")).IsInvalidArgument());
  LAMINAR_CHECK(Error(DB::Open(std::string_view("x\0y", 3))).IsInvalidArgument());
  LAMINAR_CHECK(Error(DB::Open((path / "missing-parent").string())).IsIOError());
  LAMINAR_CHECK(Error(Inspect(path.string())).IsNotFound());
  LAMINAR_CHECK(!fs::exists(path));

  auto db = DB::Open(path.string() + "/").value();
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsAlreadyExists());
  LAMINAR_CHECK(Error(Inspect(path.string())).IsAlreadyExists());
  LAMINAR_CHECK(Error(db->Get("")).IsNotFound());
  LAMINAR_CHECK(db->Delete("").ok());
  LAMINAR_CHECK(db->Put({}, {}).ok());
  LAMINAR_CHECK_EQ(db->Get({}).value(), "");
  std::string key("\0\xff", 2);
  std::string value("\0\x80\0", 3);
  LAMINAR_CHECK(db->Put(key, value).ok());
  const std::string original_key = key;
  const std::string original_value = value;
  key[0] = 'x';
  value[0] = 'x';
  LAMINAR_CHECK_EQ(db->Get(original_key).value(), original_value);
  LAMINAR_CHECK(db->Put(original_key, "").ok());
  LAMINAR_CHECK(db->Delete("").ok());

  const auto before = Read(path / "data.log");
  const std::string too_big(kMaxKey + 1, 'k');
  LAMINAR_CHECK(db->Put(too_big, "").IsInvalidArgument());
  LAMINAR_CHECK(db->Delete(too_big).IsInvalidArgument());
  LAMINAR_CHECK(Error(db->Get(too_big)).IsInvalidArgument());
  LAMINAR_CHECK(db->Put("", std::string(kMaxValue + 1, 'v')).IsInvalidArgument());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), before);

  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(db->Put("", "").IsIOError());
  LAMINAR_CHECK(Error(db->Get("")).IsIOError());
  LAMINAR_CHECK(db->Delete("").IsIOError());
  auto next = DB::Open((path / ".").string()).value();
  LAMINAR_CHECK(Error(next->Get("")).IsNotFound());
  LAMINAR_CHECK_EQ(next->Get(original_key).value(), "");
  LAMINAR_CHECK(next->Close().ok());
  const auto info = Inspect(path.string()).value();
  LAMINAR_CHECK_EQ(info.format_version, 1U);
  LAMINAR_CHECK_EQ(info.record_count, 5U);
  LAMINAR_CHECK_EQ(info.live_keys, 1U);
  LAMINAR_CHECK_EQ(info.incomplete_tail_bytes, 0U);
  LAMINAR_CHECK_EQ(info.log_bytes, before.size());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), before);
}

void BatchAPI() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  InstallFixture(path, kFresh);
  auto db = DB::Open(path.string()).value();
  WriteBatch batch;
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK_EQ(Read(log), kFresh);

  std::string key("a\0", 2);
  std::string value("\0\xff", 2);
  LAMINAR_CHECK(batch.Put(key, value).ok());
  LAMINAR_CHECK(batch.Delete({}).ok());
  key[0] = 'b';
  value[0] = 'x';
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK_EQ(Read(log), kMixed);
  LAMINAR_CHECK_EQ(db->Get(std::string_view("a\0", 2)).value(), std::string_view("\0\xff", 2));

  LAMINAR_CHECK(batch.Put(key, value).ok());
  LAMINAR_CHECK(batch.Delete(key).ok());
  LAMINAR_CHECK(batch.Put(key, "").ok());
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK(db->Write(batch).ok());
  batch.Clear();
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK_EQ(db->Get(key).value(), "");
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(db->Write(batch).IsIOError());
  LAMINAR_CHECK_EQ(Inspect(path.string()).value().record_count, 3U);
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->Get(key).value(), "");
  LAMINAR_CHECK(Error(db->Get("")).IsNotFound());
  LAMINAR_CHECK(batch.Delete(key).ok());
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK(Error(db->Get(key)).IsNotFound());
  LAMINAR_CHECK(db->Close().ok());
}

void BatchLimits() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  InstallFixture(path, kFresh);
  auto db = DB::Open(path.string()).value();
  WriteBatch batch;
  const std::string large_key(kMaxKey + 1, 'k');
  const std::string large_value(kMaxValue + 1, 'v');
  const auto key = std::string_view(large_key).substr(1);
  const auto value = std::string_view(large_value).substr(1);
  const size_t remaining = kMaxPayload - key.size() - value.size() - 2 * kOperationBytes;
  LAMINAR_CHECK(batch.Put(key, value).ok());
  LAMINAR_CHECK(batch.Put(large_key, "").IsInvalidArgument());
  LAMINAR_CHECK(batch.Delete(large_key).IsInvalidArgument());
  LAMINAR_CHECK(batch.Put("", large_value).IsInvalidArgument());
  LAMINAR_CHECK(batch.Put("", value.substr(0, remaining + 1)).IsInvalidArgument());
  LAMINAR_CHECK(batch.Put("", value.substr(0, remaining)).ok());
  LAMINAR_CHECK(batch.Delete("").IsInvalidArgument());
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK_EQ(fs::file_size(path / "data.log"),
                   kFresh.size() + kFrameHeaderBytes + kMaxPayload);
  LAMINAR_CHECK_EQ(db->Get(key).value(), value);
  LAMINAR_CHECK_EQ(db->Get("").value(), value.substr(0, remaining));

  batch.Clear();
  for (size_t i = 0; i < kMaxOperations - 1; ++i) {
    LAMINAR_CHECK(batch.Delete("").ok());
  }
  LAMINAR_CHECK(batch.Delete(key).ok());
  LAMINAR_CHECK(batch.Put("", "excess").IsInvalidArgument());
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK(Error(db->Get("")).IsNotFound());
  LAMINAR_CHECK(Error(db->Get(key)).IsNotFound());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK_EQ(Inspect(path.string()).value().record_count, 2U);
}

void EveryTruncationByte() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  const std::array images{kFresh, kMixed, kCheckpoint, kAllDeleted};
  for (size_t fixture = 0; fixture < images.size(); ++fixture) {
    const auto image = images[fixture];
    const size_t baseline = fixture == 1 ? kFresh.size() : image.size();
    for (size_t cut = 0; cut <= image.size(); ++cut) {
      const auto prefix = image.substr(0, cut);
      InstallFixture(path, prefix);
      const auto info = Inspect(path.string());
      LAMINAR_CHECK_EQ(Read(log), prefix);
      if (cut < baseline) {
        LAMINAR_CHECK(Error(info).IsCorruption());
        LAMINAR_CHECK(Error(DB::Open(path.string())).IsCorruption());
        LAMINAR_CHECK_EQ(Read(log), prefix);
      } else {
        const size_t tail = cut < image.size() ? cut - baseline : 0;
        LAMINAR_CHECK_EQ(info.value().incomplete_tail_bytes, tail);
        LAMINAR_CHECK_EQ(info->log_bytes, cut);
        auto db = DB::Open(path.string()).value();
        LAMINAR_CHECK_EQ(Read(log), prefix.substr(0, cut - tail));
        const std::string_view key("a\0", 2);
        const auto result = db->Get(key);
        if ((fixture == 1 || fixture == 2) && cut == image.size()) {
          LAMINAR_CHECK_EQ(result.value(), std::string_view("\0\xff", 2));
        } else {
          LAMINAR_CHECK(Error(result).IsNotFound());
        }
        LAMINAR_CHECK(db->Close().ok());
      }
    }
  }
}

void CorruptExtents() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto Reject = [&](std::string_view bytes, Status::Code code = Status::Code::kCorruption) {
    InstallFixture(path, bytes);
    LAMINAR_CHECK_EQ(Error(Inspect(path.string())).code(), code);
    LAMINAR_CHECK_EQ(Error(DB::Open(path.string())).code(), code);
    LAMINAR_CHECK_EQ(Read(path / "data.log"), bytes);
  };

  auto bad = std::string(kMixed);
  bad.back() ^= 1;
  Reject(bad);
  const auto record = kMixed.substr(kFresh.size());
  Reject(std::string(kMixed) + std::string(record)); // Duplicate sequence.
  Reject(std::string(kCheckpoint) + std::string(record.substr(0, kFrameHeaderBytes))); // Old seq.
  for (const std::string_view suffix : {"X", "LX", "LMX", "LMBX", "junkLMBT"}) {
    Reject(std::string(kMixed) + std::string(suffix));
  }

  const Operation put{Tag::Put, "b", "c"};
  const auto gap = EncodeFrame(Kind::Mutation, 3, std::span(&put, 1)).value();
  Reject(std::string(kMixed) + gap);
  Reject(std::string(kMixed) + gap.substr(0, kFrameHeaderBytes));

  const auto header = DecodeFileHeader(kFresh.substr(0, kFileHeaderBytes)).value();
  Reject(EncodeFileHeader({header.id, 1, 1, std::numeric_limits<uint64_t>::max()}) + EncodeSeal(0));
  const auto row =
      kCheckpoint.substr(kFileHeaderBytes, kCheckpoint.size() - kFileHeaderBytes - kSealBytes);
  const auto payload = row.substr(kFrameHeaderBytes);
  Reject(EncodeFileHeader({header.id, 1, 2, 2 * row.size()}) + std::string(row) + std::string(row) +
         EncodeSeal(crc32c::Extend(crc32c::Value(payload), payload.data(), payload.size())));
  Reject(EncodeFileHeader({header.id, 1, 1, row.size() + 1}) + std::string(row) + "X" +
         EncodeSeal(crc32c::Value(payload)));
  bad = kCheckpoint;
  bad.back() ^= 1;
  Reject(bad);
  bad = kFresh;
  bad[8] = 2;
  std::string crc;
  PutFixed32(&crc, crc32c::Value(std::string_view(bad).substr(0, 60)));
  bad.replace(60, 4, crc);
  Reject(bad, Status::Code::kNotSupported);

  const std::string_view old_key("a\0", 2);
  const std::array ops{Operation{Tag::Delete, old_key, {}}, put, Operation{Tag::Put, "d", ""}};
  const auto next = EncodeFrame(Kind::Mutation, 2, ops).value();
  for (size_t cut = 1; cut <= next.size(); ++cut) {
    InstallFixture(path, std::string(kMixed) + next.substr(0, cut));
    const bool complete = cut == next.size();
    LAMINAR_CHECK_EQ(Inspect(path.string()).value().incomplete_tail_bytes, complete ? 0U : cut);
    auto db = DB::Open(path.string()).value();
    if (complete) {
      LAMINAR_CHECK(Error(db->Get(old_key)).IsNotFound());
      LAMINAR_CHECK_EQ(db->Get("b").value(), "c");
      LAMINAR_CHECK_EQ(db->Get("d").value(), "");
    } else {
      LAMINAR_CHECK_EQ(db->Get(old_key).value(), std::string_view("\0\xff", 2));
      LAMINAR_CHECK(Error(db->Get("b")).IsNotFound());
      LAMINAR_CHECK(Error(db->Get("d")).IsNotFound());
      LAMINAR_CHECK_EQ(Read(path / "data.log"), kMixed);
    }
    LAMINAR_CHECK(db->Close().ok());
  }
}

void CheckpointAndBatchReplay() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  InstallFixture(path, kAllDeleted);
  const std::array ops{Operation{Tag::Put, "key", "one"}, Operation{Tag::Delete, "key", {}},
                       Operation{Tag::Put, "key", "two"}, Operation{Tag::Put, "", "separate"}};
  const auto batch = EncodeFrame(Kind::Mutation, 3, ops).value();
  Write(path / "data.log", std::string(kAllDeleted) + batch);
  auto db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->Get("key").value(), "two");
  LAMINAR_CHECK_EQ(db->Get("").value(), "separate");
  LAMINAR_CHECK(db->Put("next", "three").ok());
  LAMINAR_CHECK(db->Close().ok());
  const auto bytes = Read(path / "data.log");
  LAMINAR_CHECK(std::string_view(bytes).starts_with(kAllDeleted));
  const auto next = DecodeFrameHeader(std::string_view(bytes).substr(
                                          kAllDeleted.size() + batch.size(), kFrameHeaderBytes),
                                      Kind::Mutation)
                        .value();
  LAMINAR_CHECK_EQ(next.sequence, 4U);

  const auto header = DecodeFileHeader(kFresh.substr(0, kFileHeaderBytes)).value();
  constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
  const auto near_max = EncodeFileHeader({header.id, max - 1, 0, 0}) + EncodeSeal(0);
  Write(path / "data.log", near_max);
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK(db->Put("", "").ok());
  const auto at_max = Read(path / "data.log");
  LAMINAR_CHECK_EQ(
      DecodeFrameHeader(std::string_view(at_max).substr(near_max.size(), 32), Kind::Mutation)
          .value()
          .sequence,
      max);
  LAMINAR_CHECK(db->Delete("").IsFull());
  WriteBatch writes;
  LAMINAR_CHECK(db->Write(writes).ok());
  LAMINAR_CHECK(writes.Put("next", "too late").ok());
  LAMINAR_CHECK(db->Write(writes).IsFull());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), at_max);
  LAMINAR_CHECK(db->Close().ok());
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->Get("").value(), "");
  LAMINAR_CHECK(db->Put("", "").IsFull());
  LAMINAR_CHECK(db->Close().ok());
  Write(path / "data.log", at_max + "L");
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsCorruption());
}

void NamespaceAndInspect() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  fs::create_directory(path);
  const auto link = scratch.root / "alias";
  fs::create_directory_symlink(path, link);
  LAMINAR_CHECK(Error(DB::Open(link.string() + "/")).IsIOError());
  LAMINAR_CHECK(Error(Inspect(path.string())).IsNotFound());
  LAMINAR_CHECK(!fs::exists(path / "LOCK"));

  const auto foreign = path / "foreign";
  const auto temp = path / "create.tmp";
  const auto compact = path / "compact.tmp";
  const auto log = path / "data.log";
  Write(foreign, "other");
  Write(compact, kMixed);
  fs::create_symlink(foreign, temp);
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsAlreadyExists());
  LAMINAR_CHECK(fs::is_symlink(temp));
  fs::remove(temp);
  Write(temp, "LMNX");
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsAlreadyExists());
  LAMINAR_CHECK_EQ(Read(temp), "LMNX");
  Write(temp, "LM");
  LAMINAR_CHECK(Error(Inspect(path.string())).IsNotFound());
  LAMINAR_CHECK_EQ(Read(temp), "LM");
  auto db = DB::Open(path.string()).value();
  const auto initial = Read(log);
  LAMINAR_CHECK_EQ(initial.size(), kFresh.size());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(!fs::exists(temp));
  LAMINAR_CHECK_EQ(Read(foreign), "other");
  LAMINAR_CHECK_EQ(Read(compact), kMixed);
  Write(temp, "keep when data exists");
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK_EQ(Read(log), initial);
  LAMINAR_CHECK_EQ(Read(temp), "keep when data exists");

  fs::remove(log);
  fs::create_symlink(foreign, log);
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
  LAMINAR_CHECK(Error(Inspect(path.string())).IsIOError());
  LAMINAR_CHECK_EQ(Read(foreign), "other");
  fs::remove(log);
  fs::create_directory(log);
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
  fs::remove(path / "LOCK");
  fs::create_symlink(foreign, path / "LOCK");
  LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
  LAMINAR_CHECK(Error(Inspect(path.string())).IsIOError());
  LAMINAR_CHECK(fs::is_symlink(path / "LOCK"));
}

void IndexedReadsFailClosed() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  InstallFixture(path, kMixed);
  auto db = DB::Open(path.string()).value();
  auto changed = std::string(kMixed);
  changed.back() ^= 1;
  Write(log, changed);
  LAMINAR_CHECK(Error(db->Get(std::string_view("a\0", 2))).IsCorruption());
  LAMINAR_CHECK(db->Put("next", "").IsIOError());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK_EQ(Read(log), changed);

  InstallFixture(path, kMixed);
  db = DB::Open(path.string()).value();
  fs::resize_file(log, kMixed.size() - 1);
  LAMINAR_CHECK(Error(db->Get(std::string_view("a\0", 2))).IsCorruption());
  LAMINAR_CHECK(Error(db->Get("next")).IsIOError());
  LAMINAR_CHECK(db->Close().ok());
}

using ModelValue = std::optional<std::string>;

bool Matches(const Result<std::string>& got, const ModelValue& expected) {
  return expected ? got && *got == *expected : !got && got.error().IsNotFound();
}

void SharedKeyHistories() {
  const Scratch scratch;
  constexpr std::array keys{"a", "b"};
  constexpr size_t calls = 6;
  struct Event {
    size_t start{};
    size_t end{};
    Status write;
    Result<std::string> read;
  };
  for (size_t round = 0; round < 32; ++round) {
    auto db = DB::Open((scratch.root / std::to_string(round)).string()).value();
    std::array<WriteBatch, 2> batches;
    for (size_t i = 0; i < batches.size(); ++i) {
      LAMINAR_CHECK(batches[i].Put(keys[i], keys[i]).ok());
      LAMINAR_CHECK(batches[i].Delete(keys[1 - i]).ok());
    }
    std::barrier start(4);
    std::atomic<size_t> clock{0};
    std::array<Event, calls> events;
    const auto RunCall = [&](size_t i) {
      events[i].start = ++clock;
      std::this_thread::yield();
      if (i < batches.size()) {
        events[i].write = db->Write(batches[i]);
      } else {
        events[i].read = db->Get(keys[i % keys.size()]);
      }
      events[i].end = ++clock;
    };
    std::thread left([&] {
      start.arrive_and_wait();
      RunCall(0);
    });
    std::thread right([&] {
      start.arrive_and_wait();
      RunCall(1);
    });
    std::thread reader([&] {
      start.arrive_and_wait();
      RunCall(2);
      RunCall(3);
    });
    start.arrive_and_wait();
    RunCall(4);
    RunCall(5);
    left.join();
    right.join();
    reader.join();
    LAMINAR_CHECK(events[0].write.ok());
    LAMINAR_CHECK(events[1].write.ok());
    const std::array final{db->Get(keys[0]), db->Get(keys[1])};

    std::array<size_t, calls> order{0, 1, 2, 3, 4, 5};
    const auto Legal = [&] {
      std::array<ModelValue, 2> state;
      for (size_t i = 0; i < order.size(); ++i) {
        const size_t call = order[i];
        for (size_t later = i + 1; later < order.size(); ++later) {
          if (events[order[later]].end < events[call].start) {
            return false;
          }
        }
        if (call < batches.size()) {
          state[call] = keys[call];
          state[1 - call].reset();
        } else if (!Matches(events[call].read, state[call % keys.size()])) {
          return false;
        }
      }
      return Matches(final[0], state[0]) && Matches(final[1], state[1]);
    };
    while (!Legal()) {
      LAMINAR_CHECK(std::next_permutation(order.begin(), order.end()));
    }
    LAMINAR_CHECK(db->Close().ok());
  }
}

struct PublicationReceipt {
  size_t* next;
  size_t* sequence;
};

thread_local PublicationReceipt receipt{};

void SeededSharedKeyStress() {
  const Scratch scratch;
  constexpr size_t callers = 4;
  constexpr size_t commits = 4'096;
  constexpr std::array keys{"a", "b", "c", "d", "e", "f", "g", "h"};
  struct Change {
    size_t put;
    size_t erase;
    std::string value;
    size_t sequence{};
    bool ok{};
  };
  for (const uint32_t seed : {0x42415443U, 0x53484152U, 70401U, 70402U}) {
    std::cout << "shared-key seed " << seed << ", " << commits << " commits\n";
    const auto directory = (scratch.root / std::to_string(seed)).string();
    auto db = DB::Open(directory).value();
    std::mt19937 rng(seed);
    std::vector<Change> changes;
    changes.reserve(commits);
    for (size_t i = 0; i < commits; ++i) {
      changes.push_back({rng() % keys.size(), rng() % keys.size(), std::to_string(i)});
    }

    size_t published = 0;
    std::barrier start(static_cast<std::ptrdiff_t>(callers));
    std::array<std::thread, callers> threads;
    for (size_t worker = 0; worker < callers; ++worker) {
      threads[worker] = std::thread([&, worker] {
        fault_hook = [](Point point, std::string_view) noexcept -> Fault {
          if (point == Point::AfterPublish) {
            // Both the counter and receipt are written under the DB mutex.
            *receipt.sequence = ++*receipt.next;
          }
          return {};
        };
        WriteBatch batch;
        start.arrive_and_wait();
        for (size_t i = worker; i < changes.size(); i += callers) {
          auto& change = changes[i];
          batch.Clear();
          receipt = {&published, &change.sequence};
          change.ok = batch.Put(keys[change.put], change.value).ok() &&
                      batch.Delete(keys[change.erase]).ok() && db->Write(batch).ok();
          const auto got = db->Get(keys[change.put]);
          change.ok = change.ok && (got || got.error().IsNotFound());
        }
        fault_hook = nullptr;
      });
    }
    for (auto& thread : threads) {
      thread.join();
    }
    LAMINAR_CHECK_EQ(published, commits);
    std::ranges::sort(changes, {}, &Change::sequence);
    std::array<ModelValue, keys.size()> state;
    for (size_t i = 0; i < changes.size(); ++i) {
      const auto& change = changes[i];
      LAMINAR_CHECK(change.ok);
      LAMINAR_CHECK_EQ(change.sequence, i + 1);
      state[change.put] = change.value;
      state[change.erase].reset();
    }
    for (size_t i = 0; i < keys.size(); ++i) {
      LAMINAR_CHECK(Matches(db->Get(keys[i]), state[i]));
    }
    LAMINAR_CHECK(db->Close().ok());
    LAMINAR_CHECK_EQ(Inspect(directory).value().record_count, commits);
    db = DB::Open(directory).value();
    for (size_t i = 0; i < keys.size(); ++i) {
      LAMINAR_CHECK(Matches(db->Get(keys[i]), state[i]));
    }
    LAMINAR_CHECK(db->Close().ok());
  }
}

void ConcurrentClose() {
  const Scratch scratch;
  for (size_t round = 0; round < 16; ++round) {
    const auto directory = (scratch.root / std::to_string(round)).string();
    auto db = DB::Open(directory).value();
    WriteBatch batch;
    LAMINAR_CHECK(batch.Put("a", "committed").ok());
    LAMINAR_CHECK(batch.Put("b", "").ok());
    std::barrier start(4);
    Status write;
    Status closed;
    Result<std::string> read;
    std::thread writer([&] {
      start.arrive_and_wait();
      write = db->Write(batch);
    });
    std::thread reader([&] {
      start.arrive_and_wait();
      read = db->Get("a");
    });
    std::thread closer([&] {
      start.arrive_and_wait();
      closed = db->Close();
    });
    start.arrive_and_wait();
    const auto second_close = db->Close();
    writer.join();
    reader.join();
    closer.join();
    LAMINAR_CHECK(closed.ok());
    LAMINAR_CHECK(second_close.ok());
    LAMINAR_CHECK(write.ok() || write.IsIOError());
    LAMINAR_CHECK(read ? write.ok() && *read == "committed"
                       : read.error().IsNotFound() || read.error().IsIOError());
    LAMINAR_CHECK(db->Write(batch).IsIOError());

    auto reopened = DB::Open(directory).value();
    LAMINAR_CHECK(Matches(reopened->Get("a"), write.ok() ? ModelValue("committed") : std::nullopt));
    LAMINAR_CHECK(Matches(reopened->Get("b"), write.ok() ? ModelValue("") : std::nullopt));
    LAMINAR_CHECK(reopened->Close().ok());
  }
}

} // namespace

int main() {
  return laminar::test::Run({{"singleton binary API", SingletonAPI},
                             {"owned and reusable batch API", BatchAPI},
                             {"exact public batch limits", BatchLimits},
                             {"every truncation byte", EveryTruncationByte},
                             {"corrupt extents and continuity", CorruptExtents},
                             {"checkpoint and batch replay", CheckpointAndBatchReplay},
                             {"namespace and read-only inspect", NamespaceAndInspect},
                             {"indexed reads fail closed", IndexedReadsFailClosed},
                             {"shared-key serial histories", SharedKeyHistories},
                             {"seeded shared-key stress", SeededSharedKeyStress},
                             {"concurrent close", ConcurrentClose}});
}
