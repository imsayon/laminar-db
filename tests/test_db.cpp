#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#include "laminar/coding.h"
#include "laminar/crc32c.h"
#include "laminar/db.h"
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

  const auto next = EncodeFrame(Kind::Mutation, 2, std::span(&put, 1)).value();
  for (size_t cut = 1; cut < next.size(); ++cut) {
    InstallFixture(path, std::string(kMixed) + next.substr(0, cut));
    LAMINAR_CHECK_EQ(Inspect(path.string()).value().incomplete_tail_bytes, cut);
    auto db = DB::Open(path.string()).value();
    LAMINAR_CHECK_EQ(db->Get(std::string_view("a\0", 2)).value(), std::string_view("\0\xff", 2));
    LAMINAR_CHECK_EQ(Read(path / "data.log"), kMixed);
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

} // namespace

int main() {
  return laminar::test::Run({{"singleton binary API", SingletonAPI},
                             {"every truncation byte", EveryTruncationByte},
                             {"corrupt extents and continuity", CorruptExtents},
                             {"checkpoint and batch replay", CheckpointAndBatchReplay},
                             {"namespace and read-only inspect", NamespaceAndInspect},
                             {"indexed reads fail closed", IndexedReadsFailClosed}});
}
