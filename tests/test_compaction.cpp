#include <array>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unordered_map>

#include "laminar/db.h"
#include "storage/format.h"
#include "storage_test_support.h"
#include "v1_fixtures.h"

namespace {
using namespace laminar;
using namespace laminar::storage;
using namespace laminar::test;
namespace fs = std::filesystem;

FileHeader Header(const fs::path& path) {
  return DecodeFileHeader(Read(path / "data.log").substr(0, kFileHeaderBytes)).value();
}

void GoldenAndContinuity() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto directory = path.string();
  LAMINAR_CHECK(Compact("").IsInvalidArgument());
  LAMINAR_CHECK(Compact(std::string_view("x\0y", 3)).IsInvalidArgument());
  LAMINAR_CHECK(Compact(directory).IsNotFound());
  LAMINAR_CHECK(!fs::exists(path));
  InstallFixture(path, kFresh);
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), kFresh);
  Write(path / "data.log", kMixed);
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), kCheckpoint);
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), kCheckpoint);
  auto db = DB::Open(directory).value();
  const std::string key("a\0", 2);
  LAMINAR_CHECK_EQ(db->Get(key).value(), std::string("\0\xff", 2));
  LAMINAR_CHECK(Compact(directory).IsAlreadyExists());
  LAMINAR_CHECK(db->Delete(key).ok());
  LAMINAR_CHECK(db->Close().ok());
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Read(path / "data.log"), kAllDeleted);
  db = DB::Open(directory).value();
  LAMINAR_CHECK(Error(db->Get(key)).IsNotFound());
  LAMINAR_CHECK(db->Put("", "").ok());
  LAMINAR_CHECK(db->Close().ok());
  const auto bytes = Read(path / "data.log");
  const auto frame =
      DecodeFrameHeader(std::string_view(bytes).substr(kAllDeleted.size(), kFrameHeaderBytes),
                        Kind::Mutation)
          .value();
  LAMINAR_CHECK_EQ(frame.sequence, 3U);
  // A lagging cursor (same identity, sequence 1) is below the persisted replay floor.
  LAMINAR_CHECK_EQ(Header(path).floor, 2U);
  LAMINAR_CHECK_EQ(Header(path).id, DecodeFileHeader(kFresh.substr(0, 64))->id);
  auto max = Header(path);
  max.floor = std::numeric_limits<uint64_t>::max();
  Write(path / "data.log", EncodeFileHeader(max) + EncodeSeal(0));
  LAMINAR_CHECK(Compact(directory).ok());
  LAMINAR_CHECK_EQ(Header(path).floor, max.floor);
  db = DB::Open(directory).value();
  LAMINAR_CHECK(db->Put("key", "value").IsFull());
  LAMINAR_CHECK(db->Close().ok());
}

void NamespaceAndValidation() {
  const Scratch scratch;
  const auto path = scratch.root / "db";
  const auto directory = path.string();
  const auto log = path / "data.log";
  const auto temp = path / "compact.tmp";
  InstallFixture(path, kMixed);
  struct stat before {
  }, after{};
  LAMINAR_CHECK_EQ(::stat((path / "LOCK").c_str(), &before), 0);
  Write(path / "unrelated", "keep me");
  Write(path / "create.tmp", "unrecognized creation artifact");
  for (const auto prefix : {std::string_view{}, std::string_view("L"), kFileMagic, kCheckpoint}) {
    Write(temp, prefix);
    LAMINAR_CHECK(Compact(directory).ok());
    LAMINAR_CHECK(!fs::exists(temp));
  }
  for (const auto conflict : {"X", "LMNX", "user file"}) {
    Write(temp, conflict);
    const auto old = Read(log);
    LAMINAR_CHECK(Compact(directory).IsAlreadyExists());
    LAMINAR_CHECK_EQ(Read(temp), conflict);
    LAMINAR_CHECK_EQ(Read(log), old);
  }
  fs::remove(temp);
  fs::create_symlink("unrelated", temp);
  LAMINAR_CHECK(Compact(directory).IsAlreadyExists());
  LAMINAR_CHECK(fs::is_symlink(temp));
  fs::remove(temp);
  fs::create_directory(temp);
  LAMINAR_CHECK(Compact(directory).IsAlreadyExists());
  fs::remove(temp);
  LAMINAR_CHECK_EQ(::stat((path / "LOCK").c_str(), &after), 0);
  LAMINAR_CHECK_EQ(before.st_ino, after.st_ino);
  LAMINAR_CHECK_EQ(Read(path / "unrelated"), "keep me");
  LAMINAR_CHECK_EQ(Read(path / "create.tmp"), "unrecognized creation artifact");

  // Source validation precedes all temp cleanup, including a recognizable orphan.
  Write(temp, kFileMagic);
  for (size_t cut = 0; cut < kCheckpoint.size(); ++cut) {
    Write(log, kCheckpoint.substr(0, cut));
    LAMINAR_CHECK(Compact(directory).IsCorruption());
    LAMINAR_CHECK_EQ(Read(log), kCheckpoint.substr(0, cut));
    LAMINAR_CHECK_EQ(Read(temp), kFileMagic);
  }
  for (size_t byte = 0; byte < kMixed.size(); ++byte) {
    auto corrupt = std::string(kMixed);
    corrupt[byte] ^= 1;
    Write(log, corrupt);
    LAMINAR_CHECK(Compact(directory).IsCorruption());
    LAMINAR_CHECK_EQ(Read(log), corrupt);
    LAMINAR_CHECK_EQ(Read(temp), kFileMagic);
  }
  for (size_t cut = kFresh.size(); cut < kMixed.size(); ++cut) {
    Write(log, kMixed.substr(0, cut));
    LAMINAR_CHECK(Compact(directory).ok());
    LAMINAR_CHECK_EQ(Read(log), kFresh);
  }
  fs::remove(log);
  Write(temp, kFileMagic);
  LAMINAR_CHECK(Compact(directory).IsNotFound());
  LAMINAR_CHECK_EQ(Read(temp), kFileMagic);
}

void ModelAndSpace() {
  const Scratch scratch;
  for (const uint32_t seed : {70301U, 70302U}) {
    const auto path = scratch.root / std::to_string(seed);
    const auto directory = path.string();
    auto db = DB::Open(directory).value();
    const auto identity = Header(path).id;
    std::unordered_map<std::string, std::string> model;
    std::mt19937 rng(seed);
    uint64_t watermark = 0;
    for (size_t iteration = 0; iteration < 500; ++iteration) {
      WriteBatch batch;
      for (size_t op = 0; op < 8; ++op) {
        const std::string key{static_cast<char>(rng() % 16), '\0'};
        if (rng() % 3 == 0) {
          LAMINAR_CHECK(batch.Delete(key).ok());
          model.erase(key);
        } else {
          const std::string value(rng() % 128, static_cast<char>(rng() & 0xffU));
          LAMINAR_CHECK(batch.Put(key, value).ok());
          model[key] = value;
        }
      }
      LAMINAR_CHECK(db->Write(batch).ok());
      ++watermark;
      if ((iteration + 1) % 50 == 0) {
        LAMINAR_CHECK(db->Close().ok());
        const auto old_size = fs::file_size(path / "data.log");
        LAMINAR_CHECK(Compact(directory).ok());
        const auto new_size = fs::file_size(path / "data.log");
        std::cout << "seed=" << seed << " W=" << watermark << " old=" << old_size
                  << " output/temporary=" << new_size << " reclaimed="
                  << static_cast<int64_t>(old_size) - static_cast<int64_t>(new_size) << '\n';
        LAMINAR_CHECK_EQ(Header(path).floor, watermark);
        LAMINAR_CHECK_EQ(Header(path).id, identity);
        LAMINAR_CHECK_EQ(Header(path).row_count, model.size());
        db = DB::Open(directory).value();
        for (char byte = 0; byte < 16; ++byte) {
          const std::string key{byte, '\0'};
          const auto got = db->Get(key);
          if (model.contains(key)) {
            LAMINAR_CHECK_EQ(got.value(), model.at(key));
          } else {
            LAMINAR_CHECK(Error(got).IsNotFound());
          }
        }
      }
    }
    LAMINAR_CHECK(db->Close().ok());
  }
  const auto path = scratch.root / "growth";
  auto db = DB::Open(path.string()).value();
  WriteBatch batch;
  for (const auto key : {"a", "b", "c", "d"}) {
    LAMINAR_CHECK(batch.Put(key, "").ok());
  }
  LAMINAR_CHECK(db->Write(batch).ok());
  LAMINAR_CHECK(db->Close().ok());
  const auto old = fs::file_size(path / "data.log");
  LAMINAR_CHECK(Compact(path.string()).ok());
  const auto compacted = fs::file_size(path / "data.log");
  LAMINAR_CHECK_EQ(compacted - old, 3 * kFrameHeaderBytes);
  std::cout << "grouped log old=" << old << " output/temporary=" << compacted << " reclaimed=-"
            << compacted - old << '\n';
}
} // namespace

int main() {
  return Run({{"compaction golden bytes and sequence continuity", GoldenAndContinuity},
              {"compaction namespace and fail-closed validation", NamespaceAndValidation},
              {"compaction reference map and space", ModelAndSpace}});
}
