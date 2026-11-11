#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <latch>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <unordered_map>

#include "laminar/db.h"
#include "storage/file.h"
#include "storage/format.h"
#include "storage_test_support.h"
#include "v1_fixtures.h"

namespace {

thread_local int allocations_before_failure = -1;
constexpr int kFailEveryAllocation = -2;

} // namespace

void* operator new(std::size_t size) {
  if (allocations_before_failure == kFailEveryAllocation) {
    throw std::bad_alloc{};
  }
  if (allocations_before_failure == 0) {
    allocations_before_failure = -1;
    throw std::bad_alloc{};
  }
  if (allocations_before_failure > 0) {
    --allocations_before_failure;
  }
  if (void* allocation = std::malloc(size == 0 ? 1 : size)) {
    return allocation;
  }
  throw std::bad_alloc{};
}

[[gnu::noinline]] void operator delete(void* allocation) noexcept { std::free(allocation); }
[[gnu::noinline]] void operator delete(void* allocation, std::size_t) noexcept {
  std::free(allocation);
}

namespace {

using namespace laminar;
using namespace laminar::storage;
using namespace laminar::test;
namespace fs = std::filesystem;

struct Plan {
  Point point;
  std::string_view target;
  std::span<const Fault> steps{};
  size_t skip{};
  bool repeat{};
  bool stop{};
  int fail_allocation{-1};
  size_t calls{};
};

thread_local Plan* active_plan = nullptr;

Fault Inject(Point point, std::string_view target) noexcept {
  auto& plan = *active_plan;
  if (plan.point != point || plan.target != target) {
    return {};
  }
  const size_t call = plan.calls++;
  if (call < plan.skip) {
    return {};
  }
  const size_t index = call - plan.skip;
  if (index < plan.steps.size()) {
    return plan.steps[index];
  }
  if (plan.stop) {
    ::raise(SIGSTOP);
  }
  if (plan.fail_allocation >= 0) {
    allocations_before_failure = plan.fail_allocation;
  }
  return plan.repeat ? plan.steps.back() : Fault{};
}

class WithFault {
public:
  explicit WithFault(Plan& plan) {
    active_plan = &plan;
    fault_hook = Inject;
  }
  ~WithFault() {
    fault_hook = nullptr;
    active_plan = nullptr;
    allocations_before_failure = -1;
  }
};

size_t DescriptorCount() {
  return static_cast<size_t>(
      std::distance(fs::directory_iterator("/proc/self/fd"), fs::directory_iterator{}));
}

void TransferAndOffsetHandling() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  const std::array short_io{Fault{EINTR}, Fault{0, 1}, Fault{0, 4}};
  Plan random{Point::Random, "db_id", short_io};
  auto db = [&] {
    const WithFault inject(random);
    return DB::Open(path.string()).value();
  }();
  LAMINAR_CHECK_EQ(random.calls, 4U);
  const std::string key("key\0", 4);
  {
    Plan plan{Point::Write, "data.log", short_io};
    const WithFault inject(plan);
    LAMINAR_CHECK(db->Put(key, "payload").ok());
    LAMINAR_CHECK_EQ(plan.calls, 4U);
  }
  {
    Plan plan{Point::Read, "data.log", short_io};
    const WithFault inject(plan);
    LAMINAR_CHECK_EQ(db->Get(key).value(), "payload");
    LAMINAR_CHECK_EQ(plan.calls, 5U);
  }
  LAMINAR_CHECK(db->Close().ok());

  auto file = File::OpenAt(AT_FDCWD, log.c_str(), "data.log", O_RDWR | O_NOFOLLOW).value();
  std::array<char, 4> bytes{};
  LAMINAR_CHECK(Error(file.Read(kMaxOffset, bytes)).IsIOError());
  LAMINAR_CHECK(Error(file.Read(kMaxOffset + 1, {})).IsIOError());
  LAMINAR_CHECK(file.Truncate(kMaxOffset + 1).IsIOError());
  LAMINAR_CHECK(file.WriteAt(kMaxOffset, "x").IsIOError());
  LAMINAR_CHECK(file.WriteAt(kMaxOffset + 1, {}).IsIOError());
  LAMINAR_CHECK_EQ(file.Read(file.Size().value(), bytes).value(), 0U);
  LAMINAR_CHECK((::fcntl(file.fd(), F_GETFD) & FD_CLOEXEC) != 0);
  LAMINAR_CHECK(file.Close().ok());
}

void CreationFailures() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  const auto temp = path / "create.tmp";
  const size_t descriptors = DescriptorCount();
  const std::array io_error{Fault{EIO}};
  for (const auto& [point, target, installed, skip] : std::array{
           std::tuple{Point::Mkdir, "DB directory", false, 0U},
           std::tuple{Point::Open, "LOCK", false, 0U}, std::tuple{Point::Stat, "LOCK", false, 0U},
           std::tuple{Point::Lock, "LOCK", false, 0U},
           std::tuple{Point::Random, "db_id", false, 0U},
           std::tuple{Point::Open, "create.tmp", false, 1U},
           std::tuple{Point::Write, "create.tmp", false, 0U},
           std::tuple{Point::Sync, "create.tmp", false, 0U},
           std::tuple{Point::Close, "create.tmp", false, 0U},
           std::tuple{Point::Rename, "create.tmp", false, 0U},
           std::tuple{Point::Open, "data.log", true, 1U},
           std::tuple{Point::DataSync, "data.log", true, 0U},
           std::tuple{Point::Sync, "DB directory", true, 0U},
           std::tuple{Point::Sync, "parent directory", true, 0U},
           std::tuple{Point::Close, "DB directory", true, 0U}}) {
    fs::remove_all(path);
    Plan plan{point, target, io_error, skip};
    {
      const WithFault inject(plan);
      LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
    }
    LAMINAR_CHECK_EQ(plan.calls, skip + 1U);
    LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
    LAMINAR_CHECK_EQ(fs::exists(log), installed);
    const auto existing = installed ? Read(log) : std::string{};
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK(db->Close().ok());
    LAMINAR_CHECK(!fs::exists(temp));
    if (installed) {
      LAMINAR_CHECK_EQ(Read(log), existing);
    }
  }

  fs::remove(log);
  Write(temp, "LMNRDATA");
  Plan unlink{Point::Unlink, "create.tmp", io_error};
  {
    const WithFault inject(unlink);
    LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
  }
  LAMINAR_CHECK_EQ(Read(temp), "LMNRDATA");
  LAMINAR_CHECK(!fs::exists(log));
  const std::array short_eof{Fault{0, 0}};
  Plan temp_eof{Point::Read, "create.tmp", short_eof};
  {
    const WithFault inject(temp_eof);
    LAMINAR_CHECK(Error(DB::Open(path.string())).IsCorruption());
  }
  LAMINAR_CHECK_EQ(Read(temp), "LMNRDATA");
  LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
}

void RecoveryFailures() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  const size_t descriptors = DescriptorCount();
  const auto partial = kMixed.substr(0, kMixed.size() - 1);
  const std::array io_error{Fault{EIO}};
  for (const auto& [point, skip] : std::array{
           std::pair{Point::Open, 0U}, std::pair{Point::Read, 0U}, std::pair{Point::Read, 3U},
           std::pair{Point::Truncate, 0U}, std::pair{Point::DataSync, 0U}}) {
    InstallFixture(path, partial);
    Plan plan{point, "data.log", io_error, skip, true};
    {
      const WithFault inject(plan);
      LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
      LAMINAR_CHECK(Error(DB::Open(path.string())).IsIOError());
    }
    LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
    LAMINAR_CHECK_EQ(Read(log), point == Point::DataSync ? kFresh : partial);
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK_EQ(Read(log), kFresh);
    LAMINAR_CHECK(db->Close().ok());
  }

  InstallFixture(path, partial);
  const std::array short_eof{Fault{0, 0}};
  Plan unexpected_eof{Point::Read, "data.log", short_eof, 3};
  {
    const WithFault inject(unexpected_eof);
    LAMINAR_CHECK(Error(DB::Open(path.string())).IsCorruption());
  }
  LAMINAR_CHECK_EQ(Read(log), partial);
  fault_hook = [](Point point, std::string_view) noexcept -> Fault {
    switch (point) {
    case Point::Truncate:
    case Point::Sync:
    case Point::DataSync:
    case Point::Write:
    case Point::Mkdir:
    case Point::Unlink:
    case Point::Rename:
      return {EIO};
    default:
      return {};
    }
  };
  const auto inspected = Inspect(path.string());
  fault_hook = nullptr;
  LAMINAR_CHECK_EQ(inspected.value().incomplete_tail_bytes, 53U);
  LAMINAR_CHECK_EQ(Read(log), partial);
  LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
}

void AmbiguousWrites() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  const size_t descriptors = DescriptorCount();
  WriteBatch batch;
  LAMINAR_CHECK(batch.Delete("old").ok());
  LAMINAR_CHECK(batch.Put("candidate", "value").ok());
  LAMINAR_CHECK(batch.Put("other", "").ok());
  const std::array no_transfer{Fault{0, 0}};
  const std::array partial{Fault{0, 3}, Fault{ENOSPC}};
  const std::array payload_cut{Fault{0, 46}, Fault{ENOSPC}};
  const std::array append_fail{Fault{EDQUOT}};
  const std::array sync_fail{Fault{EIO}};
  for (const auto& [point, faults, tail, committed] :
       std::array{std::tuple{Point::Write, std::span<const Fault>(no_transfer), 0U, false},
                  std::tuple{Point::Write, std::span<const Fault>(partial), 3U, false},
                  std::tuple{Point::Write, std::span<const Fault>(payload_cut), 46U, false},
                  std::tuple{Point::Write, std::span<const Fault>(append_fail), 0U, false},
                  std::tuple{Point::DataSync, std::span<const Fault>(sync_fail), 0U, true}}) {
    InstallFixture(path, kFresh);
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK(db->Put("old", "kept").ok());
    Plan plan{point, "data.log", faults};
    {
      const WithFault inject(plan);
      const auto status = db->Write(batch);
      LAMINAR_CHECK(status.IsIOError());
      LAMINAR_CHECK(status.message().find("may already be committed") != std::string::npos);
      LAMINAR_CHECK(db->Put("later", "blocked").IsIOError());
      LAMINAR_CHECK(db->Delete("old").IsIOError());
      LAMINAR_CHECK(Error(db->Get("old")).IsIOError());
      LAMINAR_CHECK(db->Write(WriteBatch{}).IsIOError());
    }
    LAMINAR_CHECK_EQ(plan.calls, faults.size());
    LAMINAR_CHECK(db->Close().ok());
    LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
    const auto disk = Read(log);
    LAMINAR_CHECK_EQ(Inspect(path.string()).value().incomplete_tail_bytes, tail);
    LAMINAR_CHECK_EQ(Read(log), disk);
    db = DB::Open(path.string()).value();
    if (committed) {
      LAMINAR_CHECK(Error(db->Get("old")).IsNotFound());
      LAMINAR_CHECK_EQ(db->Get("candidate").value(), "value");
      LAMINAR_CHECK_EQ(db->Get("other").value(), "");
    } else {
      LAMINAR_CHECK_EQ(db->Get("old").value(), "kept");
      LAMINAR_CHECK(Error(db->Get("candidate")).IsNotFound());
      LAMINAR_CHECK(Error(db->Get("other")).IsNotFound());
    }
    LAMINAR_CHECK(Error(db->Get("later")).IsNotFound());
    LAMINAR_CHECK_EQ(Read(log), disk.substr(0, disk.size() - tail));
    LAMINAR_CHECK(db->Put("new", "").ok());
    LAMINAR_CHECK(db->Close().ok());
  }
}

void BatchAllocationAndSync() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  InstallFixture(path, kFresh);
  auto db = DB::Open(path.string()).value();
  WriteBatch batch;
  const std::string key(48, 'k');
  const std::string value(512, 'v');
  LAMINAR_CHECK(batch.Put("retained", "a").ok());
  for (const int fail_at : {0, 1, 2}) {
    allocations_before_failure = fail_at;
    const auto status = batch.Put(key, value);
    allocations_before_failure = -1;
    LAMINAR_CHECK(status.IsFull());
  }
  LAMINAR_CHECK(batch.Put("other", "b").ok());

  Plan sync{Point::DataSync, "data.log"};
  {
    const WithFault inject(sync);
    LAMINAR_CHECK(db->Write(WriteBatch{}).ok());
    LAMINAR_CHECK_EQ(sync.calls, 0U);
    allocations_before_failure = 0;
    const auto status = db->Write(batch);
    LAMINAR_CHECK(status.IsFull());
    LAMINAR_CHECK_EQ(sync.calls, 0U);
    LAMINAR_CHECK_EQ(Read(path / "data.log"), kFresh);
    LAMINAR_CHECK(db->Write(batch).ok());
    LAMINAR_CHECK_EQ(sync.calls, 1U);
    LAMINAR_CHECK(db->Put("single", "").ok());
    LAMINAR_CHECK_EQ(sync.calls, 2U);
    LAMINAR_CHECK(db->Delete("single").ok());
    LAMINAR_CHECK_EQ(sync.calls, 3U);
  }
  LAMINAR_CHECK_EQ(db->Get("retained").value(), "a");
  LAMINAR_CHECK_EQ(db->Get("other").value(), "b");
  LAMINAR_CHECK(Error(db->Get(key)).IsNotFound());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK_EQ(Inspect(path.string()).value().record_count, 3U);

  InstallFixture(path, kFresh);
  db = DB::Open(path.string()).value();
  // The first insertion into an empty index needs a node and a bucket array.
  Plan buckets{Point::BeforePublish, "data.log", {}, 0, false, false, 1};
  {
    const WithFault inject(buckets);
    LAMINAR_CHECK(db->Write(batch).IsFull());
    LAMINAR_CHECK(Error(db->Get("retained")).IsIOError());
  }
  LAMINAR_CHECK(db->Close().ok());
  db = DB::Open(path.string()).value();
  LAMINAR_CHECK_EQ(db->Get("retained").value(), "a");
  LAMINAR_CHECK_EQ(db->Get("other").value(), "b");
  LAMINAR_CHECK(db->Close().ok());
}

struct PublicationGate {
  std::latch reached{1};
  std::latch release{1};
  int allocation_failure;
  bool paused{};
};

thread_local PublicationGate* publication_gate = nullptr;

void PublicationBarrier() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  for (const int allocation_failure : {-1, 0, kFailEveryAllocation}) {
    InstallFixture(path, kFresh);
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK(db->Put("old", "prior").ok());
    WriteBatch batch;
    LAMINAR_CHECK(batch.Delete("old").ok());
    LAMINAR_CHECK(batch.Put("new", "value").ok());
    PublicationGate gate{.allocation_failure = allocation_failure};
    Status written;
    std::thread writer([&] {
      publication_gate = &gate;
      fault_hook = [](Point point, std::string_view) noexcept -> Fault {
        auto& active = *publication_gate;
        if (point == Point::AfterApply && !active.paused) {
          active.paused = true;
          active.reached.count_down();
          active.release.wait();
          allocations_before_failure = active.allocation_failure;
        }
        return {};
      };
      written = db->Write(batch);
      allocations_before_failure = -1;
      fault_hook = nullptr;
    });
    gate.reached.wait();
    std::latch reading(1);
    auto reader = std::async(std::launch::async, [&] {
      reading.count_down();
      return std::array{db->Get("old"), db->Get("new")};
    });
    reading.wait();
    const auto waiting = reader.wait_for(std::chrono::milliseconds(20));
    gate.release.count_down();
    writer.join();
    const auto reads = reader.get();
    LAMINAR_CHECK_EQ(waiting, std::future_status::timeout);
    if (allocation_failure != -1) {
      LAMINAR_CHECK(written.IsFull());
      if (allocation_failure == kFailEveryAllocation) {
        LAMINAR_CHECK(written.message().empty());
      } else {
        LAMINAR_CHECK(written.message().find("may already be committed") != std::string::npos);
      }
      LAMINAR_CHECK(Error(reads[0]).IsIOError());
      LAMINAR_CHECK(Error(reads[1]).IsIOError());
      LAMINAR_CHECK(db->Write(batch).IsIOError());
    } else {
      LAMINAR_CHECK(written.ok());
      LAMINAR_CHECK(Error(reads[0]).IsNotFound());
      LAMINAR_CHECK_EQ(reads[1].value(), "value");
    }
    LAMINAR_CHECK(db->Close().ok());
    db = DB::Open(path.string()).value();
    LAMINAR_CHECK(Error(db->Get("old")).IsNotFound());
    LAMINAR_CHECK_EQ(db->Get("new").value(), "value");
    LAMINAR_CHECK(db->Close().ok());
  }
}

void AllocationAndReadFailures() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto log = path / "data.log";
  auto db = DB::Open(path.string()).value();
  const std::string key(48, 'k');
  const std::string value(512, 'v');
  LAMINAR_CHECK(db->Put(key, value).ok());
  const auto before = Read(log);

  allocations_before_failure = 0;
  const auto encoding = db->Put("new", value);
  LAMINAR_CHECK(encoding.IsFull());
  LAMINAR_CHECK_EQ(Read(log), before);
  allocations_before_failure = 2; // Scratch and operation vector succeed. The returned copy fails.
  const auto copy = db->Get(key);
  LAMINAR_CHECK(Error(copy).IsFull());
  LAMINAR_CHECK_EQ(db->Get(key).value(), value);

  Plan publish{Point::BeforePublish, "data.log", {}, 0, false, false, 0};
  {
    const WithFault inject(publish);
    const auto status = db->Put("new", value);
    LAMINAR_CHECK(status.IsFull());
    LAMINAR_CHECK(status.message().find("may already be committed") != std::string::npos);
    LAMINAR_CHECK(Error(db->Get(key)).IsIOError());
    LAMINAR_CHECK(db->Delete(key).IsIOError());
  }
  LAMINAR_CHECK(db->Close().ok());

  const auto directory = path.string();
  const size_t descriptors = DescriptorCount();
  Plan replay{Point::Read, "data.log", {}, 3, false, false, 1};
  {
    const WithFault inject(replay);
    LAMINAR_CHECK(Error(DB::Open(directory)).IsFull());
  }
  LAMINAR_CHECK_EQ(replay.calls, 4U);
  LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
  db = DB::Open(directory).value();
  LAMINAR_CHECK_EQ(db->Get("new").value(), value);

  fault_hook = [](Point point, std::string_view) noexcept -> Fault {
    if (point == Point::Read) {
      allocations_before_failure = 0;
      return {EIO};
    }
    return {};
  };
  const auto read = db->Get(key);
  fault_hook = nullptr;
  LAMINAR_CHECK(Error(read).IsIOError());
  LAMINAR_CHECK(Error(read).message().empty());
  LAMINAR_CHECK(db->Put("blocked", value).IsIOError());
  LAMINAR_CHECK(db->Close().ok());
}

void ExplicitCloseErrors() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const size_t descriptors = DescriptorCount();
  for (const std::string_view label : {"data.log", "LOCK"}) {
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK(db->Put("key", "value").ok());
    const std::array interrupted{Fault{EINTR}};
    Plan close{Point::Close, label, interrupted, 0, true};
    {
      const WithFault inject(close);
      LAMINAR_CHECK(db->Close().IsIOError());
      LAMINAR_CHECK(db->Close().ok());
    }
    LAMINAR_CHECK_EQ(close.calls, 1U);
    LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
    LAMINAR_CHECK(Error(db->Get("key")).IsIOError());
    db = DB::Open(path.string()).value();
    LAMINAR_CHECK_EQ(db->Get("key").value(), "value");
    LAMINAR_CHECK(db->Close().ok());
  }
}

void KillDuring(const std::string& directory, Plan plan, const WriteBatch* batch = nullptr,
                bool compact = false) {
  const pid_t child = ::fork();
  LAMINAR_CHECK(child >= 0);
  if (child == 0) {
    if (compact) {
      const WithFault inject(plan);
      (void)Compact(directory);
    } else if (batch == nullptr) {
      const WithFault inject(plan);
      (void)DB::Open(directory);
    } else {
      auto db = DB::Open(directory);
      if (db) {
        const WithFault inject(plan);
        (void)(*db)->Write(*batch);
      }
    }
    ::_exit(99);
  }

  int state = 0;
  LAMINAR_CHECK_EQ(::waitpid(child, &state, WUNTRACED), child);
  LAMINAR_CHECK(WIFSTOPPED(state));
  const auto busy = DB::Open(directory);
  const int killed = ::kill(child, SIGKILL);
  LAMINAR_CHECK_EQ(::waitpid(child, &state, 0), child);
  LAMINAR_CHECK_EQ(killed, 0);
  LAMINAR_CHECK(WIFSIGNALED(state));
  LAMINAR_CHECK_EQ(WTERMSIG(state), SIGKILL);
  LAMINAR_CHECK(Error(busy).IsAlreadyExists());
}

void VerifyInFreshProcess(
    const std::string& directory,
    std::initializer_list<std::pair<std::string_view, std::optional<std::string_view>>> expected) {
  const pid_t child = ::fork();
  LAMINAR_CHECK(child >= 0);
  if (child == 0) {
    auto db = DB::Open(directory);
    if (!db) {
      ::_exit(1);
    }
    for (const auto& [key, value] : expected) {
      const auto got = (*db)->Get(key);
      const bool match = value ? got && *got == *value : !got && got.error().IsNotFound();
      if (!match) {
        ::_exit(2);
      }
    }
    ::_exit((*db)->Close().ok() ? 0 : 3);
  }
  int state = 0;
  LAMINAR_CHECK_EQ(::waitpid(child, &state, 0), child);
  LAMINAR_CHECK(WIFEXITED(state));
  LAMINAR_CHECK_EQ(WEXITSTATUS(state), 0);
}

void KillAtCreationBoundaries() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto directory = path.string();
  const auto log = path / "data.log";
  const std::array short_magic{Fault{0, 3}};
  const std::array header_only{Fault{0, 64}};
  for (const auto& [point, target, installed, steps] : std::array{
           std::tuple{Point::Write, "create.tmp", false, std::span<const Fault>{}},
           std::tuple{Point::Write, "create.tmp", false, std::span<const Fault>(short_magic)},
           std::tuple{Point::Write, "create.tmp", false, std::span<const Fault>(header_only)},
           std::tuple{Point::Sync, "create.tmp", false, std::span<const Fault>{}},
           std::tuple{Point::Close, "create.tmp", false, std::span<const Fault>{}},
           std::tuple{Point::Rename, "create.tmp", false, std::span<const Fault>{}},
           std::tuple{Point::DataSync, "data.log", true, std::span<const Fault>{}},
           std::tuple{Point::Sync, "DB directory", true, std::span<const Fault>{}},
           std::tuple{Point::Sync, "parent directory", true, std::span<const Fault>{}},
           std::tuple{Point::Close, "DB directory", true, std::span<const Fault>{}}}) {
    fs::remove_all(path);
    KillDuring(directory, {point, target, steps, 0, false, true});
    LAMINAR_CHECK_EQ(fs::exists(log), installed);
    const auto initial = installed ? Read(log) : std::string{};
    if (!installed) {
      LAMINAR_CHECK(fs::exists(path / "create.tmp"));
      LAMINAR_CHECK(Error(Inspect(directory)).IsNotFound());
    }
    VerifyInFreshProcess(directory, {{"", std::nullopt}});
    LAMINAR_CHECK(!fs::exists(path / "create.tmp"));
    LAMINAR_CHECK_EQ(Inspect(directory).value().record_count, 0U);
    if (installed) {
      LAMINAR_CHECK_EQ(Read(log), initial);
    }
  }
}

void KillAtMutationBoundaries() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto directory = path.string();
  const std::array partial{Fault{0, 17}};
  const std::array partial_payload{Fault{0, 46}};
  WriteBatch batch;
  LAMINAR_CHECK(batch.Delete("old").ok());
  LAMINAR_CHECK(batch.Put("new", "value").ok());
  LAMINAR_CHECK(batch.Put("other", "").ok());
  for (const auto& [point, steps, tail, committed] :
       std::array{std::tuple{Point::Write, std::span<const Fault>{}, 0U, false},
                  std::tuple{Point::Write, std::span<const Fault>(partial), 17U, false},
                  std::tuple{Point::Write, std::span<const Fault>(partial_payload), 46U, false},
                  std::tuple{Point::DataSync, std::span<const Fault>{}, 0U, true},
                  std::tuple{Point::BeforePublish, std::span<const Fault>{}, 0U, true},
                  std::tuple{Point::AfterApply, std::span<const Fault>{}, 0U, true},
                  std::tuple{Point::AfterPublish, std::span<const Fault>{}, 0U, true}}) {
    InstallFixture(path, kFresh);
    auto db = DB::Open(directory).value();
    LAMINAR_CHECK(db->Put("old", "kept").ok());
    LAMINAR_CHECK(db->Close().ok());

    KillDuring(directory, {point, "data.log", steps, 0, false, true}, &batch);
    const auto log = path / "data.log";
    const auto bytes = Read(log);
    LAMINAR_CHECK_EQ(Inspect(directory).value().incomplete_tail_bytes, tail);
    LAMINAR_CHECK_EQ(Read(log), bytes);
    VerifyInFreshProcess(
        directory, {{"old", committed ? std::nullopt : std::optional<std::string_view>("kept")},
                    {"new", committed ? std::optional<std::string_view>("value") : std::nullopt},
                    {"other", committed ? std::optional<std::string_view>("") : std::nullopt}});
    LAMINAR_CHECK_EQ(Read(log), bytes.substr(0, bytes.size() - tail));
  }
}

void ReferenceMapAcrossReopens() {
  const Scratch scratch;
  constexpr std::array<uint32_t, 4> seeds{0x4c4d4e52, 0x52454331, 70403U, 70404U};
  const std::array partial{Fault{0, 5}};
  for (const uint32_t seed : seeds) {
    std::cout << "model seed " << seed << ", 6000 operations\n";
    const auto directory = (scratch.root / std::to_string(seed)).string();
    std::mt19937 rng(seed);
    std::array<std::string, 128> keys;
    for (size_t i = 1; i < keys.size(); ++i) {
      keys[i] = {static_cast<char>(i), '\0', static_cast<char>(rng() & 0xffU)};
    }
    std::unordered_map<std::string, std::string> model;
    uint64_t writes = 0;
    auto db = DB::Open(directory).value();
    WriteBatch batch;
    const auto CheckValue = [&](const std::string& key) {
      const auto got = db->Get(key);
      const auto entry = model.find(key);
      if (entry == model.end()) {
        LAMINAR_CHECK(Error(got).IsNotFound());
      } else {
        LAMINAR_CHECK_EQ(got.value(), entry->second);
      }
    };
    for (size_t i = 0; i < 6'000; ++i) {
      const auto index = rng() % keys.size();
      const auto& key = keys[index];
      const auto action = rng() % 4;
      if (action == 0) {
        std::string value(rng() % 65, '\0');
        for (char& byte : value) {
          byte = static_cast<char>(rng() & 0xffU);
        }
        if (i % 4 == 0) {
          const auto& erased = keys[(index + 1) % keys.size()];
          batch.Clear();
          LAMINAR_CHECK(batch.Put(key, value).ok());
          LAMINAR_CHECK(batch.Delete(erased).ok());
          LAMINAR_CHECK(db->Write(batch).ok());
          model.erase(erased);
        } else {
          LAMINAR_CHECK(db->Put(key, value).ok());
        }
        model[key] = std::move(value);
        ++writes;
      } else if (action == 1) {
        LAMINAR_CHECK(db->Delete(key).ok());
        model.erase(key);
        ++writes;
      } else {
        CheckValue(key);
      }
      if ((i + 1) % 500 == 0) {
        // Mix definite pre-append OOM with uncertain sync/publication and partial append failures.
        const auto& failed_key = keys[rng() % keys.size()];
        const std::string failed_value{static_cast<char>(rng() & 0xffU), '\0'};
        batch.Clear();
        LAMINAR_CHECK(batch.Delete(failed_key).ok());
        LAMINAR_CHECK(batch.Put(failed_key, failed_value).ok());
        const size_t failure = (i / 500) % 4;
        const std::array io_error{Fault{EIO}};
        const std::array append_error{Fault{0, 5}, Fault{ENOSPC}};
        Plan plan = failure == 1 ? Plan{Point::Write, "data.log", append_error}
                                 : Plan{Point::DataSync, "data.log", io_error};
        if (failure == 3) {
          plan = {Point::BeforePublish, "data.log", {}, 0, false, false, 0};
        }
        Status failed;
        {
          const WithFault inject(plan);
          if (failure == 0) {
            allocations_before_failure = 0;
          }
          failed = db->Write(batch);
        }
        LAMINAR_CHECK(failure == 0 || failure == 3 ? failed.IsFull() : failed.IsIOError());
        if (failure >= 2) {
          model[failed_key] = failed_value;
          ++writes;
        }
        if (failure == 0) {
          CheckValue(failed_key);
        } else {
          LAMINAR_CHECK(Error(db->Get(failed_key)).IsIOError());
        }
        LAMINAR_CHECK(db->Close().ok());
        db = DB::Open(directory).value();
        for (const auto& candidate : keys) {
          CheckValue(candidate);
        }
        LAMINAR_CHECK(db->Close().ok());
        const auto interrupted = rng() % keys.size();
        const auto& interrupted_key = keys[interrupted];
        const auto& erased = keys[(interrupted + 1) % keys.size()];
        const std::string new_value{static_cast<char>(rng() & 0xffU), '\0'};
        const bool complete = rng() % 2 == 0;
        const Plan kill = complete ? Plan{Point::BeforePublish, "data.log", {}, 0, false, true}
                                   : Plan{Point::Write, "data.log", partial, 0, false, true};
        batch.Clear();
        LAMINAR_CHECK(batch.Delete(erased).ok());
        LAMINAR_CHECK(batch.Put(interrupted_key, new_value).ok());
        KillDuring(directory, kill, &batch);
        if (complete) {
          model.erase(erased);
          model[interrupted_key] = new_value;
          ++writes;
        }
        const auto log = fs::path(directory) / "data.log";
        const auto before_inspect = Read(log);
        const auto info = Inspect(directory).value();
        LAMINAR_CHECK_EQ(Read(log), before_inspect);
        LAMINAR_CHECK_EQ(info.record_count, writes);
        LAMINAR_CHECK_EQ(info.incomplete_tail_bytes, complete ? 0U : 5U);
        LAMINAR_CHECK_EQ(info.live_keys, model.size());
        db = DB::Open(directory).value();
        for (const auto& candidate : keys) {
          CheckValue(candidate);
        }
        if ((i + 1) % 1'000 == 0) {
          LAMINAR_CHECK(db->Close().ok());
          LAMINAR_CHECK(Compact(directory).ok());
          LAMINAR_CHECK(!fs::exists(fs::path(directory) / "compact.tmp"));
          writes = 0;
          db = DB::Open(directory).value();
          for (const auto& candidate : keys) {
            CheckValue(candidate);
          }
        }
      }
    }
    LAMINAR_CHECK(db->Close().ok());
  }
}

void CompactionFailuresAndKills() {
  const Scratch scratch;
  const auto path = scratch.root / "compact";
  const auto directory = path.string();
  const auto log = path / "data.log";
  const auto temp = path / "compact.tmp";
  InstallFixture(path, kMixed);
  const size_t descriptors = DescriptorCount();
  const auto reset = [&] {
    Write(log, kMixed);
    Write(temp, kFileMagic);
  };
  size_t failures = 0;
  size_t kills = 0;
  for (const auto& [point, target] : std::array{std::pair{Point::Open, "parent directory"},
                                                std::pair{Point::Open, "DB directory"},
                                                std::pair{Point::Open, "LOCK"},
                                                std::pair{Point::Open, "data.log"},
                                                std::pair{Point::Open, "compact.tmp"},
                                                std::pair{Point::Stat, "parent directory"},
                                                std::pair{Point::Stat, "DB directory"},
                                                std::pair{Point::Stat, "LOCK"},
                                                std::pair{Point::Stat, "data.log"},
                                                std::pair{Point::Stat, "compact.tmp"},
                                                std::pair{Point::Lock, "LOCK"},
                                                std::pair{Point::Read, "data.log"},
                                                std::pair{Point::Read, "compact.tmp"},
                                                std::pair{Point::Write, "compact.tmp"},
                                                std::pair{Point::PositionedWrite, "compact.tmp"},
                                                std::pair{Point::Sync, "compact.tmp"},
                                                std::pair{Point::Close, "compact.tmp"},
                                                std::pair{Point::Unlink, "compact.tmp"},
                                                std::pair{Point::Rename, "compact.tmp"},
                                                std::pair{Point::Sync, "DB directory"},
                                                std::pair{Point::Close, "data.log"},
                                                std::pair{Point::Close, "parent directory"},
                                                std::pair{Point::Close, "DB directory"},
                                                std::pair{Point::Close, "LOCK"}}) {
    reset();
    Plan count{point, target};
    {
      const WithFault inject(count);
      LAMINAR_CHECK(Compact(directory).ok());
    }
    LAMINAR_CHECK(count.calls > 0);
    for (size_t nth = 0; nth < count.calls; ++nth) {
      reset();
      const std::array error{Fault{EIO}};
      Plan plan{point, target, error, nth};
      Status failed;
      {
        const WithFault inject(plan);
        failed = Compact(directory);
      }
      LAMINAR_CHECK(failed.IsIOError());
      const bool installed = (point == Point::Sync && target == std::string_view("DB directory")) ||
                             (point == Point::Close && target != std::string_view("compact.tmp"));
      LAMINAR_CHECK_EQ(Read(log), installed ? kCheckpoint : kMixed);
      if (installed) {
        LAMINAR_CHECK(failed.message().find(point == Point::Sync ? "uncertain" : "installed") !=
                      std::string::npos);
      }
      LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
      VerifyInFreshProcess(directory, {{std::string_view("a\0", 2), std::string_view("\0\xff", 2)},
                                       {"", std::nullopt}});
      ++failures;

      // Stop at each real syscall boundary without destructor cleanup, then kill the owner.
      reset();
      const pid_t child = ::fork();
      LAMINAR_CHECK(child >= 0);
      if (child == 0) {
        Plan stop{point, target, {}, nth, false, true};
        const WithFault inject(stop);
        (void)Compact(directory);
        ::_exit(99);
      }
      int state = 0;
      LAMINAR_CHECK_EQ(::waitpid(child, &state, WUNTRACED), child);
      LAMINAR_CHECK(WIFSTOPPED(state));
      const auto visible = Read(log);
      LAMINAR_CHECK_EQ(visible, installed ? kCheckpoint : kMixed);
      if (point == Point::Rename) {
        LAMINAR_CHECK(Compact(directory).IsAlreadyExists());
        LAMINAR_CHECK(Error(Inspect(directory)).IsAlreadyExists());
      }
      LAMINAR_CHECK_EQ(::kill(child, SIGKILL), 0);
      LAMINAR_CHECK_EQ(::waitpid(child, &state, 0), child);
      LAMINAR_CHECK(WIFSIGNALED(state));
      LAMINAR_CHECK_EQ(WTERMSIG(state), SIGKILL);
      const bool orphan = fs::exists(temp);
      const auto orphan_bytes = orphan ? Read(temp) : std::string{};
      VerifyInFreshProcess(directory, {{std::string_view("a\0", 2), std::string_view("\0\xff", 2)},
                                       {"", std::nullopt}});
      LAMINAR_CHECK_EQ(Read(log), visible);
      LAMINAR_CHECK_EQ(fs::exists(temp), orphan);
      if (orphan) {
        LAMINAR_CHECK_EQ(Read(temp), orphan_bytes);
      }
      LAMINAR_CHECK(Compact(directory).ok());
      LAMINAR_CHECK_EQ(Read(log), kCheckpoint);
      LAMINAR_CHECK(!fs::exists(temp));
      ++kills;
    }
  }
  std::cout << "compaction injected failures=" << failures << " SIGKILL boundaries=" << kills
            << '\n';

  // Real partial transfers and EINTR include the positioned header backfill.
  const std::array transfers{Fault{EINTR}, Fault{0, 1}, Fault{0, 3}};
  for (const auto point :
       {Point::Read, Point::Write, Point::PositionedWrite, Point::Sync, Point::Rename}) {
    reset();
    Plan plan{point, "compact.tmp", transfers};
    {
      const WithFault inject(plan);
      LAMINAR_CHECK(Compact(directory).ok());
    }
    LAMINAR_CHECK_EQ(Read(log), kCheckpoint);
  }
  for (const auto point : {Point::Write, Point::PositionedWrite}) {
    for (const auto fault : {Fault{ENOSPC}, Fault{0, 0}}) {
      reset();
      const std::array steps{Fault{0, 3}, fault};
      Plan plan{point, "compact.tmp", steps};
      {
        const WithFault inject(plan);
        LAMINAR_CHECK(Compact(directory).IsIOError());
      }
      LAMINAR_CHECK_EQ(Read(log), kMixed);
      LAMINAR_CHECK(!fs::exists(temp));
    }
  }
  reset();
  fault_hook = [](Point point, std::string_view target) noexcept -> Fault {
    if (target == "compact.tmp" && point == Point::Write) {
      return {ENOSPC};
    }
    if (target == "compact.tmp" && point == Point::Unlink) {
      return {EACCES};
    }
    return {};
  };
  fs::remove(temp);
  const auto primary = Compact(directory);
  fault_hook = nullptr;
  LAMINAR_CHECK(primary.IsIOError());
  LAMINAR_CHECK(primary.message().find("errno " + std::to_string(ENOSPC)) != std::string::npos);
  LAMINAR_CHECK(fs::exists(temp));
  LAMINAR_CHECK_EQ(Read(log), kMixed);
  fs::remove(temp);

  // No short read within the captured extent is accepted, including output validation.
  for (const auto target : {"data.log", "compact.tmp"}) {
    reset();
    const std::array eof{Fault{0, 0}};
    Plan plan{Point::Read, target, eof, target == std::string_view("compact.tmp") ? 1U : 0U};
    {
      const WithFault inject(plan);
      LAMINAR_CHECK(Compact(directory).IsCorruption());
    }
    LAMINAR_CHECK_EQ(Read(log), kMixed);
  }

  for (const auto point : {Point::Write, Point::PositionedWrite}) {
    reset();
    const std::string source = std::string(kMixed) + "LMBT";
    Write(log, source);
    fs::remove(temp);
    const std::array partial{Fault{0, 3}};
    KillDuring(directory, Plan{point, "compact.tmp", partial, 0, false, true}, nullptr, true);
    LAMINAR_CHECK_EQ(Read(log), source);
    LAMINAR_CHECK(fs::exists(temp));
    LAMINAR_CHECK_EQ(Inspect(directory)->incomplete_tail_bytes, 4U);
    VerifyInFreshProcess(directory, {{std::string_view("a\0", 2), std::string_view("\0\xff", 2)}});
    LAMINAR_CHECK_EQ(Read(log), kMixed);
    LAMINAR_CHECK(Compact(directory).ok());
    LAMINAR_CHECK_EQ(Read(log), kCheckpoint);
  }
  reset();
  Write(log, std::string(kMixed) + "LMBT");
  const auto source_with_tail = Read(log);
  const std::array no_space{Fault{ENOSPC}};
  Plan failed_copy{Point::Write, "compact.tmp", no_space};
  {
    const WithFault inject(failed_copy);
    LAMINAR_CHECK(Compact(directory).IsIOError());
  }
  LAMINAR_CHECK_EQ(Read(log), source_with_tail);
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Read(log), kCheckpoint);

  size_t allocation_failures = 0;
  for (int nth = 0; nth < 200; ++nth) {
    reset();
    allocations_before_failure = nth;
    const auto result = Compact(directory);
    allocations_before_failure = -1;
    LAMINAR_CHECK_EQ(DescriptorCount(), descriptors);
    if (result.ok()) {
      break;
    }
    LAMINAR_CHECK(result.IsFull());
    LAMINAR_CHECK_EQ(Read(log), kMixed);
    ++allocation_failures;
  }
  LAMINAR_CHECK(allocation_failures > 0 && allocation_failures < 200);
  std::cout << "compaction allocation failures=" << allocation_failures << '\n';
}

} // namespace

int main() {
  return laminar::test::Run({{"partial transfers, EINTR and offsets", TransferAndOffsetHandling},
                             {"creation syscall failures", CreationFailures},
                             {"compaction faults and SIGKILL", CompactionFailuresAndKills},
                             {"recovery syscall failures", RecoveryFailures},
                             {"ambiguous writes disable handle", AmbiguousWrites},
                             {"batch allocations and one sync", BatchAllocationAndSync},
                             {"batch publication barrier", PublicationBarrier},
                             {"allocation and read failures", AllocationAndReadFailures},
                             {"explicit close errors", ExplicitCloseErrors},
                             {"SIGKILL during creation", KillAtCreationBoundaries},
                             {"SIGKILL during mutation", KillAtMutationBoundaries},
                             {"24,000-operation reference map", ReferenceMapAcrossReopens}});
}
