#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "laminar/coding.h"
#include "laminar/db.h"
#include "storage/file.h"
#include "storage/format.h"
#include "test_harness.h"

namespace {

using namespace laminar;
using namespace laminar::storage;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

struct Counters {
  size_t syncs{};
  size_t reads{};
  uint64_t temporary_bytes{};
  const char* temporary_path{};
};

thread_local Counters counters;

Fault Count(Point point, std::string_view target) noexcept {
  if (target == "data.log") {
    counters.syncs += point == Point::DataSync ? 1U : 0U;
    counters.reads += point == Point::Read ? 1U : 0U;
  }
  if (point == Point::Rename && target == "compact.tmp" && counters.temporary_path != nullptr) {
    struct stat info {};
    if (::stat(counters.temporary_path, &info) == 0) {
      counters.temporary_bytes = static_cast<uint64_t>(info.st_size);
    }
  }
  return {};
}

double Micros(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

double CPUSeconds() {
  struct timespec time {};
  LAMINAR_CHECK_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time), 0);
  return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) / 1e9;
}

long ResidentKiB() {
  std::ifstream input("/proc/self/statm");
  long pages = 0;
  long resident = 0;
  LAMINAR_CHECK(static_cast<bool>(input >> pages >> resident));
  return resident * ::sysconf(_SC_PAGESIZE) / 1024;
}

void Metric(std::string_view name, double value) {
  std::cout << "metric," << name << ',' << value << '\n';
}

void Samples(const std::vector<double>& samples, double wall_us, double cpu_seconds) {
  for (size_t i = 0; i < samples.size(); ++i) {
    std::cout << "sample," << i << ',' << samples[i] << '\n';
  }
  Metric("wall_us", wall_us);
  Metric("cpu_seconds", cpu_seconds);
}

std::string Key(size_t index) {
  std::string key;
  PutFixed64(&key, static_cast<uint64_t>(index));
  PutFixed64(&key, 70405);
  return key;
}

std::vector<std::string> Values(size_t bytes) {
  std::mt19937 rng(70405);
  std::vector<std::string> values(128, std::string(bytes, '\0'));
  for (auto& value : values) {
    for (char& byte : value) {
      byte = static_cast<char>(rng() & 255U);
    }
  }
  return values;
}

void Fill(DB& db, size_t count, size_t keyspace, const std::vector<std::string>& values) {
  WriteBatch batch;
  for (size_t i = 0; i < count; ++i) {
    LAMINAR_CHECK(batch.Put(Key(i % keyspace), values[i % values.size()]).ok());
    if ((i + 1) % 128 == 0 || i + 1 == count) {
      LAMINAR_CHECK(db.Write(batch).ok());
      batch.Clear();
    }
  }
}

void Writes(const std::string& directory, size_t count, size_t bytes, size_t batch_size) {
  const auto values = Values(bytes);
  auto db = DB::Open(directory).value();
  Fill(*db, 128, 128, values);
  counters = {};
  std::vector<double> samples(count / batch_size);
  const auto cpu = CPUSeconds();
  const auto start = Clock::now();
  WriteBatch batch;
  for (size_t i = 0; i < samples.size(); ++i) {
    batch.Clear();
    for (size_t j = 0; j < batch_size; ++j) {
      const auto index = i * batch_size + j;
      if (batch_size != 1) {
        LAMINAR_CHECK(batch.Put(Key(index), values[index % values.size()]).ok());
      }
    }
    const auto key = Key(i);
    const auto call = Clock::now();
    const auto status =
        batch_size == 1 ? db->Put(key, values[i % values.size()]) : db->Write(batch);
    samples[i] = Micros(call);
    LAMINAR_CHECK(status.ok());
  }
  const auto wall = Micros(start);
  const auto cpu_seconds = CPUSeconds() - cpu;
  LAMINAR_CHECK_EQ(counters.syncs, samples.size());
  Metric("data_syncs", static_cast<double>(counters.syncs));
  Samples(samples, wall, cpu_seconds);
  for (size_t i = 0; i < count; i += batch_size) {
    LAMINAR_CHECK_EQ(db->Get(Key(i)).value(), values[i % values.size()]);
  }
  LAMINAR_CHECK_EQ(db->Get(Key(count - 1)).value(), values[(count - 1) % values.size()]);
  LAMINAR_CHECK(db->Close().ok());
}

void Reads(const std::string& directory, size_t count, size_t bytes, size_t batch_size) {
  const auto values = Values(bytes);
  const std::string small(64, 's');
  auto db = DB::Open(directory).value();
  WriteBatch batch;
  LAMINAR_CHECK(batch.Put(Key(0), small).ok());
  for (size_t i = 1; i < batch_size; ++i) {
    LAMINAR_CHECK(batch.Put(Key(i), values[i % values.size()]).ok());
  }
  LAMINAR_CHECK(db->Write(batch).ok());
  for (size_t i = 0; i < 16; ++i) {
    LAMINAR_CHECK_EQ(db->Get(Key(0)).value(), small);
  }
  counters = {};
  std::vector<double> samples(count);
  const auto cpu = CPUSeconds();
  const auto start = Clock::now();
  const auto key = Key(0);
  for (auto& sample : samples) {
    const auto call = Clock::now();
    const auto got = db->Get(key);
    sample = Micros(call);
    LAMINAR_CHECK_EQ(got.value(), small);
  }
  const auto wall = Micros(start);
  const auto cpu_seconds = CPUSeconds() - cpu;
  LAMINAR_CHECK_EQ(counters.reads, 2 * count);
  Metric("read_calls", static_cast<double>(counters.reads));
  // Full validated frame length; this is application read traffic, not device traffic.
  Metric("frame_bytes_per_get",
         static_cast<double>(fs::file_size(fs::path(directory) / "data.log") - kFileHeaderBytes -
                             kSealBytes));
  Samples(samples, wall, cpu_seconds);
  LAMINAR_CHECK(db->Close().ok());
}

void Recovery(const std::string& directory, size_t count, size_t keyspace) {
  const auto resident_before = ResidentKiB();
  const auto cpu = CPUSeconds();
  const auto start = Clock::now();
  auto db = DB::Open(directory).value();
  const auto wall = Micros(start);
  const auto cpu_seconds = CPUSeconds() - cpu;
  const auto resident_after = ResidentKiB();
  struct rusage usage {};
  LAMINAR_CHECK_EQ(::getrusage(RUSAGE_SELF, &usage), 0);
  Metric("rss_before_kib", static_cast<double>(resident_before));
  Metric("rss_open_kib", static_cast<double>(resident_after));
  Metric("rss_delta_kib", static_cast<double>(resident_after - resident_before));
  Metric("peak_rss_kib", static_cast<double>(usage.ru_maxrss));
  Samples({wall}, wall, cpu_seconds);
  const auto values = Values(64);
  for (size_t i = 0; i < keyspace; ++i) {
    const auto last = i + ((count - 1 - i) / keyspace) * keyspace;
    LAMINAR_CHECK_EQ(db->Get(Key(i)).value(), values[last % values.size()]);
  }
  LAMINAR_CHECK(db->Close().ok());
}

void Compaction(const std::string& directory, size_t count, size_t bytes, size_t rounds) {
  const auto values = Values(bytes);
  auto db = DB::Open(directory).value();
  for (size_t round = 0; round < rounds; ++round) {
    Fill(*db, count, count, values);
  }
  if (rounds > 1) {
    WriteBatch batch;
    for (size_t i = 0; i < count; i += 4) {
      LAMINAR_CHECK(batch.Delete(Key(i)).ok());
    }
    LAMINAR_CHECK(db->Write(batch).ok());
  }
  LAMINAR_CHECK(db->Close().ok());
  const auto before = fs::file_size(fs::path(directory) / "data.log");
  const auto temporary = (fs::path(directory) / "compact.tmp").string();
  counters = {.temporary_path = temporary.c_str()};
  const auto cpu = CPUSeconds();
  const auto start = Clock::now();
  LAMINAR_CHECK(Compact(directory).ok());
  const auto wall = Micros(start);
  const auto cpu_seconds = CPUSeconds() - cpu;
  const auto after = fs::file_size(fs::path(directory) / "data.log");
  LAMINAR_CHECK_EQ(counters.temporary_bytes, after);
  Metric("before_bytes", static_cast<double>(before));
  Metric("after_bytes", static_cast<double>(after));
  Metric("temporary_bytes", static_cast<double>(counters.temporary_bytes));
  Metric("peak_logical_bytes", static_cast<double>(before + after));
  Samples({wall}, wall, cpu_seconds);
  db = DB::Open(directory).value();
  for (size_t i = 0; i < count; ++i) {
    const auto got = db->Get(Key(i));
    if (rounds > 1 && i % 4 == 0) {
      LAMINAR_CHECK(!got && got.error().IsNotFound());
    } else {
      LAMINAR_CHECK_EQ(got.value(), values[i % values.size()]);
    }
  }
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(!fs::exists(temporary));
}

void Contention(const std::string& directory, size_t count, size_t callers) {
  const auto values = Values(64);
  auto db = DB::Open(directory).value();
  Fill(*db, 128, 64, values);
  std::vector<double> samples(count);
  std::vector<std::thread> threads;
  std::array<size_t, 8> syncs{};
  std::array<int, 8> failures{};
  std::barrier ready(static_cast<std::ptrdiff_t>(callers + 1));
  std::barrier start(static_cast<std::ptrdiff_t>(callers + 1));
  for (size_t worker = 0; worker < callers; ++worker) {
    threads.emplace_back([&, worker] {
      fault_hook = Count;
      counters = {};
      ready.arrive_and_wait();
      start.arrive_and_wait();
      for (size_t i = worker; i < count; i += callers) {
        const auto key = Key(i % 64);
        const auto call = Clock::now();
        const auto status = db->Put(key, values[i % 64]);
        samples[i] = Micros(call);
        failures[worker] += status.ok() ? 0 : 1;
      }
      syncs[worker] = counters.syncs;
      fault_hook = nullptr;
    });
  }
  ready.arrive_and_wait();
  const auto cpu = CPUSeconds();
  const auto started = Clock::now();
  start.arrive_and_wait();
  for (auto& thread : threads) {
    thread.join();
  }
  const auto wall = Micros(started);
  const auto cpu_seconds = CPUSeconds() - cpu;
  size_t synced = 0;
  for (size_t worker = 0; worker < callers; ++worker) {
    LAMINAR_CHECK_EQ(failures[worker], 0);
    synced += syncs[worker];
  }
  LAMINAR_CHECK_EQ(synced, count);
  Metric("data_syncs", static_cast<double>(synced));
  Samples(samples, wall, cpu_seconds);
  for (size_t i = 0; i < 64; ++i) {
    LAMINAR_CHECK_EQ(db->Get(Key(i)).value(), values[i]);
  }
  LAMINAR_CHECK(db->Close().ok());
}

size_t Number(std::string_view text) {
  size_t number = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
  LAMINAR_CHECK(error == std::errc{} && end == text.data() + text.size());
  LAMINAR_CHECK(number > 0 && number <= 65'536);
  return number;
}

} // namespace

int main(int argc, char** argv) try {
  std::cout << std::setprecision(17);
  LAMINAR_CHECK_EQ(argc, 6);
  const std::string_view mode = argv[1];
  const std::string directory = argv[2];
  const auto count = Number(argv[3]);
  const auto bytes = Number(argv[4]);
  const auto parameter = Number(argv[5]);
  fault_hook = Count;
  if (mode == "write") {
    LAMINAR_CHECK(parameter <= 128 && count % parameter == 0 && count >= 128);
    Writes(directory, count, bytes, parameter);
  } else if (mode == "read") {
    LAMINAR_CHECK(parameter <= kMaxOperations);
    Reads(directory, count, bytes, parameter);
  } else if (mode == "prepare") {
    LAMINAR_CHECK(parameter <= count);
    const auto values = Values(64);
    auto db = DB::Open(directory).value();
    Fill(*db, count, parameter, values);
    LAMINAR_CHECK(db->Close().ok());
  } else if (mode == "recovery") {
    LAMINAR_CHECK(parameter <= count);
    Recovery(directory, count, parameter);
  } else if (mode == "compact") {
    LAMINAR_CHECK(count <= 4'096 && parameter <= 4);
    Compaction(directory, count, bytes, parameter);
  } else {
    LAMINAR_CHECK_EQ(mode, "contention");
    LAMINAR_CHECK(parameter <= 8);
    Contention(directory, count, parameter);
  }
  const auto info = Inspect(directory).value();
  Metric("log_bytes", static_cast<double>(info.log_bytes));
  Metric("live_keys", static_cast<double>(info.live_keys));
  LAMINAR_CHECK_EQ(info.incomplete_tail_bytes, 0U);
  return std::cout.good() ? 0 : 1;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
