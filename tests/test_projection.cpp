#include <atomic>
#include <limits>

#include "redis_test_support.h"
#include "storage/format.h"

namespace {
using namespace laminar;
using namespace laminar::projection;
using namespace laminar::test;
using storage::kFrameHeaderBytes;
using storage::kOperationBytes;

CommittedBatch Batch(ChangeCursor before, std::vector<Change> ops) {
  ++before.sequence;
  size_t bytes = kFrameHeaderBytes;
  for (const auto& op : ops) {
    bytes += kOperationBytes + op.key.size() + op.value.size();
  }
  return {before, std::move(ops), bytes};
}

void OrderedApplyAndValidation() {
  RedisServer server;
  ChangeCursor initial;
  initial.db_id[0] = 'x';
  initial.sequence = (uint64_t{1} << 53) + 7;
  auto projection = RedisProjection::Open(server.socket, "ordered").value();
  LAMINAR_CHECK(!projection->Cursor());
  LAMINAR_CHECK(!projection->Get("key"));
  // A controlled empty fixture establishes the test epoch; production recovery compares source
  // state.
  LAMINAR_CHECK(TestAccess::Publish(*projection, initial).ok());
  const std::string key("\0\xff", 2), value("a\0b", 3);
  const auto batch = Batch(initial, {{Change::Type::Put, key, "old"},
                                     {Change::Type::Delete, key, ""},
                                     {Change::Type::Put, key, value},
                                     {Change::Type::Put, "", ""}});
  LAMINAR_CHECK(projection->Apply(batch).ok());
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK_EQ(projection->Get("").value(), "");
  LAMINAR_CHECK_EQ(projection->Cursor().value(), batch.cursor);
  LAMINAR_CHECK(projection->Apply(batch).ok());
  auto earlier = batch;
  earlier.cursor.sequence = initial.sequence;
  LAMINAR_CHECK(projection->Apply(earlier).ok());
  auto invalid = Batch(batch.cursor, {{Change::Type::Put, key, "bad"}});
  ++invalid.cursor.sequence;
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid.cursor = batch.cursor;
  invalid.cursor.db_id[0] ^= 1;
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) -- malformed input regression.
  invalid = Batch(batch.cursor, {{static_cast<Change::Type>(3), key, "bad"}});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor, {{Change::Type::Delete, key, "bad"}});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor, {});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor, {{Change::Type::Put, std::string(storage::kMaxKey + 1, 'k'), ""}});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid =
      Batch(batch.cursor, {{Change::Type::Put, key, std::string(storage::kMaxValue + 1, 'v')}});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor, {{Change::Type::Put, key, ""}});
  ++invalid.encoded_bytes;
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor,
                  std::vector<Change>(storage::kMaxOperations + 1, {Change::Type::Delete, "", ""}));
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  invalid = Batch(batch.cursor, {{Change::Type::Put, "a", std::string(storage::kMaxValue, 'v')},
                                 {Change::Type::Put, "b", std::string(storage::kMaxValue, 'v')}});
  LAMINAR_CHECK(projection->Apply(invalid).IsInvalidArgument());
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:ordered:meta", "seq"}).value(),
                   SequenceHex(batch.cursor.sequence));
  LAMINAR_CHECK_EQ(SequenceHex(std::numeric_limits<uint64_t>::max()), "ffffffffffffffff");
  LAMINAR_CHECK_EQ(SequenceHex(0), "0000000000000000");
  auto restarted = RedisProjection::Open(server.socket, "ordered").value();
  LAMINAR_CHECK(!restarted->Get(key));
  LAMINAR_CHECK(!restarted->Apply(batch).ok());
  LAMINAR_CHECK(Error(RedisProjection::Open(server.socket, "*")).IsInvalidArgument());
  initial.sequence = std::numeric_limits<uint64_t>::max() - 1;
  auto maximum = RedisProjection::Open(server.socket, "last-sequence").value();
  LAMINAR_CHECK(TestAccess::Publish(*maximum, initial).ok());
  const auto last = Batch(initial, {{Change::Type::Delete, "absent", ""}});
  LAMINAR_CHECK(maximum->Apply(last).ok());
  LAMINAR_CHECK_EQ(maximum->Cursor()->sequence, std::numeric_limits<uint64_t>::max());
  LAMINAR_CHECK(maximum->Apply(last).ok());
}

size_t dispatches{};
Fault LoseReply(Point point) noexcept {
  if (point == Point::Sent) {
    ++dispatches;
    return {.lose_reply = true};
  }
  return {};
}

void LostReplyAndOutage() {
  RedisServer server;
  ChangeCursor zero;
  zero.db_id[0] = 'x';
  auto projection = RedisProjection::Open(server.socket, "lost").value();
  LAMINAR_CHECK(TestAccess::Publish(*projection, zero).ok());
  const auto batch = Batch(zero, {{Change::Type::Put, "key", "value"}});
  dispatches = 0;
  fault_hook = LoseReply;
  const auto result = projection->Apply(batch);
  fault_hook = nullptr;
  LAMINAR_CHECK(result.ok());
  LAMINAR_CHECK_EQ(dispatches, 1U);
  LAMINAR_CHECK_EQ(projection->Get("key").value(), "value");
  const auto next = Batch(batch.cursor, {{Change::Type::Put, "key", "next"}});
  ::kill(server.pid, SIGSTOP);
  const auto start = std::chrono::steady_clock::now();
  const auto timeout = projection->Apply(next);
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  ::kill(server.pid, SIGCONT);
  LAMINAR_CHECK(timeout.IsIOError());
  LAMINAR_CHECK(seconds >= 1.8 && seconds < 5);
  LAMINAR_CHECK(!projection->Get("key"));
  LAMINAR_CHECK(!projection->Apply(next).ok());
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:lost:data", "key"}).value(), "value");
  auto offline = RedisProjection::Open(server.socket, "offline").value();
  LAMINAR_CHECK(TestAccess::Publish(*offline, zero).ok());
  server.Stop();
  LAMINAR_CHECK(offline->Apply(batch).IsIOError());
  LAMINAR_CHECK(!offline->Cursor());
  LAMINAR_CHECK(!projection->Get("key"));
}

std::atomic<bool> sent{false}, release{false};
Fault PartialAndWait(Point point) noexcept {
  if (point == Point::Script) {
    return {.script_error = true};
  }
  if (point == Point::Sent) {
    sent = true;
    while (!release) {
      std::this_thread::yield();
    }
  }
  return {};
}

void PartialFailureClosesAdmission() {
  RedisServer server;
  ChangeCursor zero;
  zero.db_id[0] = 'x';
  auto projection = RedisProjection::Open(server.socket, "partial").value();
  LAMINAR_CHECK(TestAccess::Publish(*projection, zero).ok());
  const auto batch =
      Batch(zero, {{Change::Type::Put, "first", "new"}, {Change::Type::Put, "second", "new"}});
  sent = false;
  release = false;
  Status status;
  std::thread writer([&] {
    fault_hook = PartialAndWait;
    status = projection->Apply(batch);
    fault_hook = nullptr;
  });
  while (!sent) {
    std::this_thread::yield();
  }
  std::atomic<bool> returned{false}, admitted{false};
  std::thread reader([&] {
    admitted = projection->Get("first").has_value();
    returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const bool escaped = returned;
  release = true;
  writer.join();
  reader.join();
  LAMINAR_CHECK(!escaped);
  LAMINAR_CHECK(!admitted);
  LAMINAR_CHECK(status.IsIOError());
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:partial:data", "first"}).value(), "new");
  LAMINAR_CHECK(!server.Run({"HGET", "laminar:partial:data", "second"}));
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:partial:meta", "seq"}).value(), SequenceHex(0));
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:partial:meta", "ready"}).value(), "0");
  LAMINAR_CHECK(!projection->Apply(batch).ok());
}

Fault PartialAndStop(Point point) noexcept {
  if (point == Point::Script) {
    return {.script_error = true};
  }
  if (point == Point::Sent) {
    ::raise(SIGSTOP);
  }
  return {};
}

void OwnerDeathBeforeErrorReply() {
  RedisServer server;
  ChangeCursor zero;
  zero.db_id[0] = 'x';
  const auto batch =
      Batch(zero, {{Change::Type::Put, "first", "new"}, {Change::Type::Put, "second", "new"}});
  const pid_t owner = ::fork();
  LAMINAR_CHECK(owner >= 0);
  if (owner == 0) {
    auto projection = RedisProjection::Open(server.socket, "killed").value();
    if (!TestAccess::Publish(*projection, zero).ok()) {
      ::_exit(2);
    }
    fault_hook = PartialAndStop;
    const auto result = projection->Apply(batch);
    ::_exit(result.ok() ? 3 : 4);
  }
  int status{};
  LAMINAR_CHECK_EQ(::waitpid(owner, &status, WUNTRACED), owner);
  const bool stopped = WIFSTOPPED(status);
  const auto first = server.Run({"HGET", "laminar:killed:data", "first"});
  const auto second = server.Run({"HGET", "laminar:killed:data", "second"});
  const auto ready = server.Run({"HGET", "laminar:killed:meta", "ready"});
  ::kill(owner, SIGKILL);
  LAMINAR_CHECK_EQ(::waitpid(owner, &status, 0), owner);
  LAMINAR_CHECK(stopped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  LAMINAR_CHECK_EQ(first.value(), "new");
  LAMINAR_CHECK(!second);
  LAMINAR_CHECK_EQ(ready.value(), "0");
  auto restarted = RedisProjection::Open(server.socket, "killed").value();
  LAMINAR_CHECK(!restarted->Get("first"));
  LAMINAR_CHECK(!restarted->Apply(batch).ok());
}

void MaximumBatch() {
  RedisServer server;
  const Scratch scratch;
  auto db = DB::Open((scratch.root / "db").string()).value();
  const auto zero = db->LatestSequence().value();
  auto projection = RedisProjection::Open(server.socket, "maximum").value();
  LAMINAR_CHECK(TestAccess::Publish(*projection, zero).ok());
  WriteBatch write;
  const std::string key(storage::kMaxKey, 'k'), value(storage::kMaxValue, 'v');
  LAMINAR_CHECK(write.Put(key, value).ok());
  const size_t remaining = storage::kMaxPayload - storage::kOperationBytes - key.size() -
                           value.size() - storage::kOperationBytes - 1 -
                           (storage::kMaxOperations - 2) * storage::kOperationBytes;
  LAMINAR_CHECK(write.Put("b", std::string(remaining, 'b')).ok());
  for (size_t i = 2; i < storage::kMaxOperations; ++i) {
    LAMINAR_CHECK(write.Delete("").ok());
  }
  LAMINAR_CHECK(db->Write(write).ok());
  const auto batch =
      db->ReadChangesAfter(zero, 1, storage::kMaxPayload + kFrameHeaderBytes)->batches[0];
  LAMINAR_CHECK_EQ(batch.encoded_bytes, storage::kMaxPayload + kFrameHeaderBytes);
  LAMINAR_CHECK_EQ(batch.operations.size(), storage::kMaxOperations);
  const auto before = server.Run({"INFO", "memory"}).value();
  const auto start = std::chrono::steady_clock::now();
  LAMINAR_CHECK(projection->Apply(batch).ok());
  const auto elapsed =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK_EQ(projection->Get("b")->size(), remaining);
  LAMINAR_CHECK(Error(projection->Get("")).IsNotFound());
  const auto memory = server.Run({"INFO", "memory"}).value();
  std::cout << "maximum_batch_bytes=" << batch.encoded_bytes
            << " operations=" << batch.operations.size() << " apply_ms=" << elapsed
            << "\nredis_memory_before\n"
            << before << "redis_memory_after\n"
            << memory;
}
} // namespace
int main() {
  return Run(
      {{"ordered apply, duplicate, gap and validation", OrderedApplyAndValidation},
       {"lost reply classification, timeout and outage", LostReplyAndOutage},
       {"partial Lua failure closes and drains read admission", PartialFailureClosesAdmission},
       {"owner killed before observing partial-script error", OwnerDeathBeforeErrorReply},
       {"maximum legal source batch on real Redis", MaximumBatch}});
}
