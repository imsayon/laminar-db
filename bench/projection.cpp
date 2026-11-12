#include <atomic>
#include <fstream>
#include <iomanip>
#include <random>
#include <sys/resource.h>

#include "laminar/coding.h"
#include "redis_test_support.h"
#include "storage/format.h"

namespace {
using namespace laminar;
using namespace laminar::test;
using namespace laminar::projection;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kSeed = 70505;
constexpr size_t kBatches = 64;
constexpr size_t kOperations = 32;
constexpr size_t kMaxFrame = storage::kMaxPayload + storage::kFrameHeaderBytes;

double Micros(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
void Metric(std::string_view name, double value) {
  std::cout << "metric," << name << ',' << value << '\n';
}
void Event(std::string_view stage, ChangeCursor cursor, uint64_t lag, size_t bytes,
           size_t pending) {
  std::cout << "event," << stage << ',' << SequenceHex(cursor.sequence) << ',' << lag << ','
            << bytes << ',' << pending << '\n';
}
long ResidentKiB() {
  std::ifstream input("/proc/self/statm");
  long pages{}, resident{};
  LAMINAR_CHECK(static_cast<bool>(input >> pages >> resident));
  return resident * ::sysconf(_SC_PAGESIZE) / 1024;
}
void Memory(const RedisServer& server, std::string_view stage) {
  const auto info = server.Run({"INFO", "memory"}).value();
  for (const auto field : {"used_memory:", "used_memory_peak:", "used_memory_rss:"}) {
    const auto start = info.find(field);
    LAMINAR_CHECK_NE(start, std::string::npos);
    const auto value = info.substr(start + std::string_view(field).size());
    Metric(std::string(stage) + "_" + std::string(field, std::string_view(field).size() - 1),
           std::stod(value));
  }
}
std::string Key(size_t index) {
  std::string key;
  PutFixed64(&key, static_cast<uint64_t>(index));
  PutFixed64(&key, kSeed);
  return key;
}
std::atomic<bool> dispatched{false};
Fault ObserveDispatch(Point point) noexcept {
  if (point == Point::Sent) {
    dispatched = true;
  }
  return {};
}

void Demonstrate() {
  RedisServer server;
  const Scratch scratch;
  const auto directory = (scratch.root / "db").string();
  auto db = DB::Open(directory).value();
  auto projection = RedisProjection::Open(server.socket, "demo").value();
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  std::mt19937 rng(kSeed);
  std::vector<std::string> values(128, std::string(1024, '\0'));
  for (auto& value : values) {
    for (char& byte : value) {
      byte = static_cast<char>(rng() & 255U);
    }
  }
  std::vector<size_t> lengths;
  for (size_t i = 0; i < kBatches + 8; ++i) {
    WriteBatch batch;
    size_t bytes = storage::kFrameHeaderBytes;
    for (size_t j = 0; j < kOperations; ++j) {
      const auto key = Key(rng() % 128);
      const bool erase = rng() % 5 == 0;
      const auto& value = values[rng() % values.size()];
      LAMINAR_CHECK((erase ? batch.Delete(key) : batch.Put(key, value)).ok());
      bytes += storage::kOperationBytes + key.size() + (erase ? 0 : value.size());
    }
    LAMINAR_CHECK(db->Write(batch).ok());
    if (i < 8) {
      const auto cursor = projection->Cursor().value();
      const auto page = db->ReadChangesAfter(cursor, 1, kMaxFrame).value();
      LAMINAR_CHECK(projection->Apply(page.batches[0]).ok());
    } else {
      lengths.push_back(bytes);
    }
  }
  size_t byte_lag{};
  for (const auto bytes : lengths) {
    byte_lag += bytes;
  }
  const auto frontier = db->LatestSequence().value();
  auto cursor = projection->Cursor().value();
  Event("durable_backlog", cursor, frontier.sequence - cursor.sequence, byte_lag, 0);
  CommittedBatch last;
  const auto replay_start = Clock::now();
  for (const auto expected : lengths) {
    const auto replay = Clock::now();
    auto page = db->ReadChangesAfter(cursor, 1, kMaxFrame).value();
    LAMINAR_CHECK_EQ(page.batches.size(), 1U);
    auto& batch = page.batches[0];
    LAMINAR_CHECK_EQ(batch.encoded_bytes, expected);
    const auto apply = Clock::now();
    LAMINAR_CHECK(projection->Apply(batch).ok());
    std::cout << "sample,apply_us," << Micros(apply) << '\n';
    std::cout << "sample,replay_us," << Micros(replay) << '\n';
    byte_lag -= batch.encoded_bytes;
    cursor = batch.cursor;
    Event("replay", cursor, frontier.sequence - cursor.sequence, byte_lag, batch.encoded_bytes);
    last = std::move(batch);
  }
  Metric("replay_total_us", Micros(replay_start));
  LAMINAR_CHECK_EQ(byte_lag, 0U);
  projection = RedisProjection::Open(server.socket, "demo").value();
  LAMINAR_CHECK(!projection->Cursor());
  auto start = Clock::now();
  LAMINAR_CHECK(projection->Reconcile(*db).ok());
  Metric("restart_reconcile_us", Micros(start));
  LAMINAR_CHECK(projection->Apply(last).ok());
  Event("restart_duplicate", projection->Cursor().value(), 0, 0, last.encoded_bytes);
  LAMINAR_CHECK(db->Put("expiry", "committed").ok());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(Compact(directory).ok());
  db = DB::Open(directory).value();
  LAMINAR_CHECK(Error(db->ReadChangesAfter(cursor, 1, kMaxFrame)).IsNotFound());
  LAMINAR_CHECK(!projection->Reconcile(*db).ok());
  Event("history_expired", cursor, 1, storage::kFrameHeaderBytes + storage::kOperationBytes + 6 + 9,
        0);
  start = Clock::now();
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  Metric("rebuild_us", Micros(start));
  LAMINAR_CHECK_EQ(projection->Get("expiry").value(), "committed");
  cursor = projection->Cursor().value();
  Event("exact_convergence", cursor, 0, 0, 0);

  Memory(server, "before_maximum");
  Metric("owner_before_maximum_rss_kib", static_cast<double>(ResidentKiB()));
  WriteBatch maximum;
  const std::string key(storage::kMaxKey, 'k'), value(storage::kMaxValue, 'v');
  LAMINAR_CHECK(maximum.Put(key, value).ok());
  const size_t remaining = storage::kMaxPayload - storage::kOperationBytes - key.size() -
                           value.size() - storage::kOperationBytes - 1 -
                           (storage::kMaxOperations - 2) * storage::kOperationBytes;
  LAMINAR_CHECK(maximum.Put("b", std::string(remaining, 'b')).ok());
  for (size_t i = 2; i < storage::kMaxOperations; ++i) {
    LAMINAR_CHECK(maximum.Delete("").ok());
  }
  LAMINAR_CHECK(db->Write(maximum).ok());
  start = Clock::now();
  const auto page = db->ReadChangesAfter(cursor, 1, kMaxFrame).value();
  Metric("maximum_pull_us", Micros(start));
  LAMINAR_CHECK_EQ(page.batches[0].encoded_bytes, kMaxFrame);
  dispatched = false;
  std::atomic<bool> done{false};
  Status applied;
  double maximum_apply{};
  std::thread writer([&] {
    fault_hook = ObserveDispatch;
    const auto apply = Clock::now();
    applied = projection->Apply(page.batches[0]);
    maximum_apply = Micros(apply);
    fault_hook = nullptr;
    done = true;
  });
  while (!dispatched && !done) {
    std::this_thread::yield();
  }
  const auto inflight_rss = ResidentKiB();
  start = Clock::now();
  const auto read = projection->Get("expiry");
  const auto blocking = Micros(start);
  writer.join();
  LAMINAR_CHECK(applied.ok());
  LAMINAR_CHECK_EQ(read.value(), "committed");
  Metric("maximum_apply_us", maximum_apply);
  Metric("maximum_concurrent_get_us", blocking);
  Metric("maximum_pending_bytes", static_cast<double>(page.batches[0].encoded_bytes));
  Metric("owner_inflight_rss_kib", static_cast<double>(inflight_rss));
  struct rusage usage {};
  LAMINAR_CHECK_EQ(::getrusage(RUSAGE_SELF, &usage), 0);
  Metric("owner_peak_rss_kib", static_cast<double>(usage.ru_maxrss));
  Memory(server, "after_maximum");
  start = Clock::now();
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  Metric("maximum_state_rebuild_us", Micros(start));
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK_EQ(projection->Get("b")->size(), remaining);
  Event("maximum_exact_convergence", projection->Cursor().value(), 0, 0, 0);
  std::cout << "config,seed," << kSeed << "\nconfig,batches," << kBatches
            << "\nconfig,operations_per_batch," << kOperations << "\n";
}
} // namespace
int main() {
  try {
    std::cout << std::setprecision(12);
    Demonstrate();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
