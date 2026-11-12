#include <atomic>

#include "redis_test_support.h"
#include "storage/format.h"

namespace {
using namespace laminar;
using namespace laminar::projection;
using namespace laminar::test;

void ExactRecovery() {
  RedisServer server;
  const Scratch scratch;
  auto db = DB::Open((scratch.root / "db").string()).value();
  const std::string key("\0\xff", 2), value("a\0b", 3);
  LAMINAR_CHECK(db->Put(key, value).ok());
  LAMINAR_CHECK(db->Put("", "").ok());
  LAMINAR_CHECK(db->Put("deleted", "old").ok());
  LAMINAR_CHECK(db->Delete("deleted").ok());
  auto projection = RedisProjection::Open(server.socket, "repair").value();
  LAMINAR_CHECK(!projection->Reconcile(*db).ok());
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  const auto watermark = db->LatestSequence().value();
  LAMINAR_CHECK_EQ(projection->Cursor().value(), watermark);
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK_EQ(projection->Get("").value(), "");
  LAMINAR_CHECK(Error(projection->Get("deleted")).IsNotFound());
  server.Run({"SET", "unrelated", "preserve"});
  server.Run({"HSET", "laminar:neighbor:data", "key", "preserve"});
  const auto repaired = [&] {
    LAMINAR_CHECK(!projection->Reconcile(*db).ok());
    LAMINAR_CHECK(!projection->Get(key));
    LAMINAR_CHECK(projection->Rebuild(*db).ok());
    LAMINAR_CHECK_EQ(projection->Cursor().value(), watermark);
    LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
    LAMINAR_CHECK_EQ(server.Run({"HLEN", "laminar:repair:data"}).value(), "2");
  };
  server.Run({"HSET", "laminar:repair:data", key, "wrong"});
  repaired();
  server.Run({"HDEL", "laminar:repair:data", key});
  repaired();
  server.Run({"HSET", "laminar:repair:data", "ghost", "extra"});
  repaired();
  // Same cardinality, different key set: a count alone must not pass.
  server.Run({"HDEL", "laminar:repair:data", key});
  server.Run({"HSET", "laminar:repair:data", "deleted", "ghost"});
  repaired();
  server.Run({"HDEL", "laminar:repair:meta", "seq"});
  repaired();
  server.Run({"HSET", "laminar:repair:meta", "seq", "0000000000000001"});
  repaired();
  server.Run({"HSET", "laminar:repair:meta", "id", std::string(32, 'f')});
  repaired();
  server.Run({"DEL", "laminar:repair:data"});
  repaired();
  server.Run({"DEL", "laminar:repair:data", "laminar:repair:meta"});
  repaired();
  server.Run({"DEL", "laminar:repair:data"});
  server.Run({"SET", "laminar:repair:data", "wrong type"});
  repaired();
  LAMINAR_CHECK_EQ(server.Run({"GET", "unrelated"}).value(), "preserve");
  LAMINAR_CHECK_EQ(server.Run({"HGET", "laminar:neighbor:data", "key"}).value(), "preserve");
  projection.reset();
  projection = RedisProjection::Open(server.socket, "repair").value();
  LAMINAR_CHECK(!projection->Get(key));
  LAMINAR_CHECK(projection->Reconcile(*db).ok());
  LAMINAR_CHECK_EQ(projection->Get(key).value(), value);
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(!projection->Rebuild(*db).ok());
  LAMINAR_CHECK(!projection->Cursor());
}

void ExpiryAndEmpty() {
  RedisServer server;
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto db = DB::Open(path.string()).value();
  auto projection = RedisProjection::Open(server.socket, "expiry").value();
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  const auto zero = projection->Cursor().value();
  LAMINAR_CHECK(db->Put("key", "first").ok());
  LAMINAR_CHECK(db->Delete("key").ok());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(Compact(path.string()).ok());
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK(Error(db->ReadChangesAfter(zero, 1, 1000)).IsNotFound());
  LAMINAR_CHECK(!projection->Reconcile(*db).ok());
  server.Run({"HSET", "laminar:expiry:data", "key", "ghost"});
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  const auto watermark = projection->Cursor().value();
  LAMINAR_CHECK_EQ(watermark.sequence, 2U);
  LAMINAR_CHECK_EQ(server.Run({"HLEN", "laminar:expiry:data"}).value(), "0");
  LAMINAR_CHECK(Error(projection->Get("key")).IsNotFound());
  LAMINAR_CHECK(db->Put("key", "after floor").ok());
  const auto page = db->ReadChangesAfter(watermark, 1, 1000).value();
  LAMINAR_CHECK_EQ(page.batches[0].cursor.sequence, 3U);
  LAMINAR_CHECK(projection->Apply(page.batches[0]).ok());
  LAMINAR_CHECK_EQ(projection->Get("key").value(), "after floor");
}

std::atomic<bool> row_written{false}, proceed{false};
Fault PauseRebuild(Point point) noexcept {
  if (point == Point::RebuildRow) {
    row_written = true;
    while (!proceed) {
      std::this_thread::yield();
    }
  }
  return {};
}

void PausedWritesAndReads() {
  RedisServer server;
  const Scratch scratch;
  auto db = DB::Open((scratch.root / "db").string()).value();
  LAMINAR_CHECK(db->Put("key", "before").ok());
  const auto watermark = db->LatestSequence().value();
  auto projection = RedisProjection::Open(server.socket, "paused").value();
  row_written = false;
  proceed = false;
  Status rebuilt, written;
  std::thread maintenance([&] {
    fault_hook = PauseRebuild;
    rebuilt = projection->Rebuild(*db);
    fault_hook = nullptr;
  });
  while (!row_written) {
    std::this_thread::yield();
  }
  std::atomic<bool> write_returned{false}, read_returned{false};
  std::thread writer([&] {
    written = db->Put("key", "after");
    write_returned = true;
  });
  std::string observed;
  std::thread reader([&] {
    observed = projection->Get("key").value_or("ERROR");
    read_returned = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const bool escaped = write_returned || read_returned;
  proceed = true;
  maintenance.join();
  writer.join();
  reader.join();
  LAMINAR_CHECK(!escaped);
  LAMINAR_CHECK(rebuilt.ok() && written.ok());
  LAMINAR_CHECK_EQ(observed, "before");
  LAMINAR_CHECK_EQ(projection->Cursor().value(), watermark);
  const auto page = db->ReadChangesAfter(watermark, 1, 1000).value();
  LAMINAR_CHECK(projection->Apply(page.batches[0]).ok());
  LAMINAR_CHECK_EQ(projection->Get("key").value(), "after");
}

Fault InterruptRebuild(Point point) noexcept {
  return {.script_error = point == Point::RebuildRow};
}
Fault FailPublication(Point point) noexcept {
  return {.script_error = point == Point::BeforeReady};
}
Fault PartialApply(Point point) noexcept { return {.script_error = point == Point::Script}; }

void InterruptedRepair() {
  RedisServer server;
  const Scratch scratch;
  auto db = DB::Open((scratch.root / "db").string()).value();
  LAMINAR_CHECK(db->Put("one", "old").ok());
  LAMINAR_CHECK(db->Put("two", "old").ok());
  auto projection = RedisProjection::Open(server.socket, "interrupted").value();
  for (const auto hook : {InterruptRebuild, FailPublication}) {
    fault_hook = hook;
    const auto failed = projection->Rebuild(*db);
    fault_hook = nullptr;
    LAMINAR_CHECK(!failed.ok());
    LAMINAR_CHECK(!projection->Get("one"));
    LAMINAR_CHECK(!projection->Reconcile(*db).ok());
    projection = RedisProjection::Open(server.socket, "interrupted").value();
    LAMINAR_CHECK(!projection->Get("one"));
    LAMINAR_CHECK(projection->Rebuild(*db).ok());
  }
  const auto before = projection->Cursor().value();
  WriteBatch batch;
  LAMINAR_CHECK(batch.Put("one", "new").ok());
  LAMINAR_CHECK(batch.Put("two", "new").ok());
  LAMINAR_CHECK(db->Write(batch).ok());
  const auto page = db->ReadChangesAfter(before, 1, 1000).value();
  fault_hook = PartialApply;
  const auto failed = projection->Apply(page.batches[0]);
  fault_hook = nullptr;
  LAMINAR_CHECK(!failed.ok());
  projection = RedisProjection::Open(server.socket, "interrupted").value();
  LAMINAR_CHECK(!projection->Get("one"));
  LAMINAR_CHECK(!projection->Reconcile(*db).ok());
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  LAMINAR_CHECK_EQ(projection->Get("one").value(), "new");
  LAMINAR_CHECK_EQ(projection->Get("two").value(), "new");
}

Fault StopPartialApply(Point point) noexcept {
  if (point == Point::Script) {
    return {.script_error = true};
  }
  if (point == Point::Sent) {
    ::raise(SIGSTOP);
  }
  return {};
}

void KilledPartialApply() {
  RedisServer server;
  const Scratch scratch;
  const auto path = scratch.root / "db";
  auto db = DB::Open(path.string()).value();
  LAMINAR_CHECK(db->Put("one", "old").ok());
  LAMINAR_CHECK(db->Put("two", "old").ok());
  auto projection = RedisProjection::Open(server.socket, "killed-apply").value();
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  const auto before = db->LatestSequence().value();
  LAMINAR_CHECK(db->Close().ok());
  const auto owner = ::fork();
  LAMINAR_CHECK(owner >= 0);
  if (owner == 0) {
    auto child_db = DB::Open(path.string()).value();
    auto child_projection = RedisProjection::Open(server.socket, "killed-apply").value();
    if (!child_projection->Reconcile(*child_db).ok()) {
      ::_exit(2);
    }
    WriteBatch batch;
    if (!batch.Put("one", "new").ok() || !batch.Put("two", "new").ok() ||
        !child_db->Write(batch).ok()) {
      ::_exit(3);
    }
    const auto page = child_db->ReadChangesAfter(before, 1, 1000).value();
    fault_hook = StopPartialApply;
    const auto result = child_projection->Apply(page.batches[0]);
    ::_exit(result.ok() ? 4 : 5);
  }
  int status{};
  LAMINAR_CHECK_EQ(::waitpid(owner, &status, WUNTRACED), owner);
  const bool stopped = WIFSTOPPED(status);
  const auto first = server.Run({"HGET", "laminar:killed-apply:data", "one"});
  const auto second = server.Run({"HGET", "laminar:killed-apply:data", "two"});
  ::kill(owner, SIGKILL);
  LAMINAR_CHECK_EQ(::waitpid(owner, &status, 0), owner);
  LAMINAR_CHECK(stopped && WIFSIGNALED(status));
  LAMINAR_CHECK_EQ(first.value(), "new");
  LAMINAR_CHECK_EQ(second.value(), "old");
  db = DB::Open(path.string()).value();
  projection = RedisProjection::Open(server.socket, "killed-apply").value();
  LAMINAR_CHECK(!projection->Get("one"));
  LAMINAR_CHECK(!projection->Reconcile(*db).ok());
  LAMINAR_CHECK(projection->Rebuild(*db).ok());
  LAMINAR_CHECK_EQ(projection->Get("one").value(), "new");
  LAMINAR_CHECK_EQ(projection->Get("two").value(), "new");
  LAMINAR_CHECK_EQ(projection->Cursor().value(), db->LatestSequence().value());
}

Point kill_point = Point::RebuildRow;
Fault StopRepair(Point point) noexcept {
  if (point == kill_point) {
    ::raise(SIGSTOP);
  }
  return {};
}

void KilledRepair() {
  for (const auto point : {Point::RebuildRow, Point::BeforeReady}) {
    RedisServer server;
    const Scratch scratch;
    const auto path = scratch.root / "db";
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK(db->Put("one", "value").ok());
    LAMINAR_CHECK(db->Put("two", "value").ok());
    const auto watermark = db->LatestSequence().value();
    LAMINAR_CHECK(db->Close().ok());
    kill_point = point;
    const auto owner = ::fork();
    LAMINAR_CHECK(owner >= 0);
    if (owner == 0) {
      auto child_db = DB::Open(path.string()).value();
      auto projection = RedisProjection::Open(server.socket, "killed-rebuild").value();
      fault_hook = StopRepair;
      const auto result = projection->Rebuild(*child_db);
      ::_exit(result.ok() ? 2 : 3);
    }
    int status{};
    LAMINAR_CHECK_EQ(::waitpid(owner, &status, WUNTRACED), owner);
    const bool stopped = WIFSTOPPED(status);
    const auto blocked = DB::Open(path.string());
    ::kill(owner, SIGKILL);
    LAMINAR_CHECK_EQ(::waitpid(owner, &status, 0), owner);
    LAMINAR_CHECK(stopped && WIFSIGNALED(status));
    LAMINAR_CHECK(Error(blocked).IsAlreadyExists());
    db = DB::Open(path.string()).value();
    auto projection = RedisProjection::Open(server.socket, "killed-rebuild").value();
    LAMINAR_CHECK(!projection->Get("one"));
    LAMINAR_CHECK(!projection->Reconcile(*db).ok());
    LAMINAR_CHECK(projection->Rebuild(*db).ok());
    LAMINAR_CHECK_EQ(projection->Cursor().value(), watermark);
    LAMINAR_CHECK_EQ(projection->Get("one").value(), "value");
    LAMINAR_CHECK_EQ(projection->Get("two").value(), "value");
  }
}
} // namespace
int main() {
  return Run({{"exact same-watermark repair and checkpoint/dataset loss", ExactRecovery},
              {"expired history and all-deleted nonzero watermark", ExpiryAndEmpty},
              {"maintenance pauses source writes and projection reads", PausedWritesAndReads},
              {"partial apply/rebuild and failed publication stay unready", InterruptedRepair},
              {"partial script then owner death and exact repair", KilledPartialApply},
              {"owner killed during rebuild and before publication", KilledRepair}});
}
