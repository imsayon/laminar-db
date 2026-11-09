#include "laminar/db.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "laminar/crc32c.h"
#include "storage/file.h"
#include "storage/format.h"

namespace laminar {
namespace {

using namespace storage;
using Code = Status::Code;

constexpr char kLog[] = "data.log";
constexpr char kLock[] = "LOCK";
constexpr char kTemp[] = "create.tmp";
constexpr char kDirectory[] = "DB directory";
constexpr char kParent[] = "parent directory";
constexpr int kDirectoryFlags = O_RDONLY | O_DIRECTORY;
constexpr uint64_t kMaxSequence = std::numeric_limits<uint64_t>::max();

Status CurrentException() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return Status::FromCode(Code::kFull);
  } catch (const std::length_error&) {
    return Status::FromCode(Code::kFull);
  } catch (...) {
    return Status::FromCode(Code::kIOError);
  }
}

Status ReopenRequired(const Status& cause) noexcept {
  try {
    const auto message =
        std::string("reopen required, mutation may already be committed: ") + cause.ToString();
    return cause.IsFull() ? Status::Full(message) : Status::IOError(message);
  } catch (...) {
    return Status::FromCode(cause.code());
  }
}

struct LockedDirectory {
  File parent;
  File dir;
  File lock;
};

Result<LockedDirectory> LockDirectory(std::string_view path, bool create) {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    return std::unexpected(Status::InvalidArgument("invalid DB directory path"));
  }

  while (path.size() > 1 && path.back() == '/') {
    path.remove_suffix(1);
  }
  const size_t slash = path.find_last_of('/');
  const std::string parent_path(slash == std::string_view::npos ? "." : path.substr(0, slash + 1));
  const std::string leaf(
      path == "/" ? "." : path.substr(slash == std::string_view::npos ? 0 : slash + 1));
  auto parent = File::OpenAt(AT_FDCWD, parent_path.c_str(), kParent, kDirectoryFlags);
  if (!parent) {
    if (create && parent.error().IsNotFound()) {
      return std::unexpected(Status::IOError(parent.error().message()));
    }
    return std::unexpected(std::move(parent).error());
  }

  const auto open_leaf = [&] {
    return File::OpenAt(parent->fd(), leaf.c_str(), kDirectory, kDirectoryFlags | O_NOFOLLOW);
  };
  auto dir = open_leaf();
  if (!dir && dir.error().IsNotFound() && create) {
    if (auto status = MakeDirectory(parent->fd(), leaf.c_str()); !status.ok()) {
      return std::unexpected(std::move(status));
    }
    auto created = open_leaf();
    if (!created) {
      return std::unexpected(std::move(created).error());
    }
    dir.emplace(std::move(*created));
  }
  if (!dir) {
    return std::unexpected(std::move(dir).error());
  }
  // Dot paths resolve to an existing directory. Sync the parent of the resolved leaf.
  if (create && (leaf == "." || leaf == "..")) {
    auto actual_parent = File::OpenAt(dir->fd(), "..", kParent, kDirectoryFlags);
    if (!actual_parent) {
      return std::unexpected(std::move(actual_parent).error());
    }
    parent.emplace(std::move(*actual_parent));
  }

  const int flags = create ? O_RDWR | O_CREAT : O_RDONLY;
  auto lock = File::OpenAt(dir->fd(), kLock, kLock, flags | O_NOFOLLOW);
  if (!lock) {
    return std::unexpected(std::move(lock).error());
  }
  if (auto status = lock->Lock(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return LockedDirectory{std::move(*parent), std::move(*dir), std::move(*lock)};
}

Status ReadExact(const File& file, uint64_t offset, std::span<char> bytes) {
  auto size = file.Read(offset, bytes);
  if (!size) {
    return std::move(size).error();
  }
  return *size == bytes.size() ? Status::OK() : Corrupt("unexpected EOF inside log extent");
}

Status RemoveStaleTemp(int directory) {
  auto temp = File::OpenAt(directory, kTemp, kTemp, O_RDONLY | O_NOFOLLOW, Code::kAlreadyExists);
  if (!temp) {
    return temp.error().IsNotFound() ? Status::OK() : std::move(temp).error();
  }
  std::array<char, kFileMagic.size()> magic{};
  auto size = temp->Size();
  if (!size) {
    return std::move(size).error();
  }
  const size_t prefix = static_cast<size_t>(std::min<uint64_t>(*size, magic.size()));
  LAMINAR_RETURN_IF_ERROR(ReadExact(*temp, 0, std::span(magic).first(prefix)));
  if (!kFileMagic.starts_with(std::string_view(magic.data(), prefix))) {
    return Status::AlreadyExists("unrecognized create.tmp");
  }
  LAMINAR_RETURN_IF_ERROR(temp->Close());
  LAMINAR_RETURN_IF_ERROR(RemoveFile(directory, kTemp));
  if (*size == 0) {
    std::fputs("LaminarDB removed empty create.tmp\n", stderr);
  }
  return Status::OK();
}

Status CreateLog(int directory) {
  LAMINAR_RETURN_IF_ERROR(RemoveStaleTemp(directory));
  FileHeader header;
  do {
    LAMINAR_RETURN_IF_ERROR(RandomBytes(header.id));
  } while (std::ranges::all_of(header.id, [](char byte) { return byte == 0; }));
  const auto bytes = EncodeFileHeader(header) + EncodeSeal(0);

  auto temp = File::OpenAt(directory, kTemp, kTemp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW);
  if (!temp) {
    return std::move(temp).error();
  }
  LAMINAR_RETURN_IF_ERROR(temp->Append(bytes));
  LAMINAR_RETURN_IF_ERROR(temp->Sync(Point::Sync));
  LAMINAR_RETURN_IF_ERROR(temp->Close());
  return InstallFile(directory, kTemp, kLog);
}

struct Entry {
  Kind kind;
  uint64_t offset;
  size_t bytes;
  size_t ordinal;
};

struct KeyHash {
  using is_transparent = void;
  size_t operator()(std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key);
  }
};
using Index = std::unordered_map<std::string, Entry, KeyHash, std::equal_to<>>;

void Apply(Index& index, const Operation& op, const Entry& entry) {
  const auto existing = index.find(op.key);
  if (op.tag == Tag::Delete) {
    if (existing != index.end()) {
      index.erase(existing);
    }
  } else if (existing != index.end()) {
    existing->second = entry;
  } else {
    index.emplace(std::string(op.key), entry);
  }
}

Result<FrameHeader> ReadFrameHeader(const File& log, uint64_t offset, Kind kind) {
  std::array<char, kFrameHeaderBytes> bytes{};
  if (auto status = ReadExact(log, offset, bytes); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return DecodeFrameHeader(std::string_view(bytes.data(), bytes.size()), kind);
}

Result<std::vector<Operation>> ReadPayload(const File& log, uint64_t offset,
                                           const FrameHeader& header, Kind kind,
                                           std::string& scratch) {
  scratch.resize(header.payload_bytes);
  if (auto status = ReadExact(log, offset + kFrameHeaderBytes, scratch); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return DecodePayload(header, kind, scratch);
}

struct Replay {
  FileHeader header;
  Index index;
  uint64_t frontier;
  uint64_t offset;
  uint64_t records{};
  uint64_t size;
};

Result<Replay> Scan(const File& log) {
  auto size = log.Size();
  if (!size) {
    return std::unexpected(std::move(size).error());
  }
  if (*size < kFileHeaderBytes) {
    return std::unexpected(Corrupt("short file header"));
  }

  std::array<char, kFileHeaderBytes> bytes{};
  if (auto status = ReadExact(log, 0, bytes); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  auto header = DecodeFileHeader(std::string_view(bytes.data(), bytes.size()));
  if (!header) {
    return std::unexpected(std::move(header).error());
  }
  if (*size - kFileHeaderBytes < kSealBytes ||
      header->row_bytes > *size - kFileHeaderBytes - kSealBytes) {
    return std::unexpected(Corrupt("incomplete baseline region or seal"));
  }
  Replay replay{*header, {}, header->floor, kFileHeaderBytes, 0, *size};
  const uint64_t rows_end = kFileHeaderBytes + header->row_bytes;
  std::string scratch;
  uint32_t seal_crc = 0;
  for (uint64_t row = 0; row < header->row_count; ++row) {
    if (rows_end - replay.offset < kFrameHeaderBytes) {
      return std::unexpected(Corrupt("short baseline frame header"));
    }
    auto frame = ReadFrameHeader(log, replay.offset, Kind::Baseline);
    if (!frame) {
      return std::unexpected(std::move(frame).error());
    }
    if (frame->payload_bytes > rows_end - replay.offset - kFrameHeaderBytes) {
      return std::unexpected(Corrupt("baseline frame exceeds row region"));
    }
    auto ops = ReadPayload(log, replay.offset, *frame, Kind::Baseline, scratch);
    if (!ops) {
      return std::unexpected(std::move(ops).error());
    }
    const size_t length = kFrameHeaderBytes + frame->payload_bytes;
    if (!replay.index
             .emplace(std::string(ops->front().key),
                      Entry{Kind::Baseline, replay.offset, length, 0})
             .second) {
      return std::unexpected(Corrupt("duplicate baseline key"));
    }
    seal_crc = crc32c::Extend(seal_crc, scratch.data(), scratch.size());
    replay.offset += length;
  }
  if (replay.offset != rows_end) {
    return std::unexpected(Corrupt("baseline row bytes mismatch"));
  }
  std::array<char, kSealBytes> seal{};
  if (auto status = ReadExact(log, replay.offset, seal); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = ValidateSeal(std::string_view(seal.data(), seal.size()), seal_crc);
      !status.ok()) {
    return std::unexpected(std::move(status));
  }
  replay.offset += kSealBytes;

  while (replay.offset < *size) {
    if (replay.frontier == kMaxSequence) {
      return std::unexpected(Corrupt("log bytes after maximum sequence"));
    }
    const uint64_t remaining = *size - replay.offset;
    if (remaining < kFrameHeaderBytes) {
      std::array<char, kFrameHeaderBytes> partial{};
      const auto observed = std::span(partial).first(static_cast<size_t>(remaining));
      if (auto status = ReadExact(log, replay.offset, observed); !status.ok()) {
        return std::unexpected(std::move(status));
      }
      const size_t prefix = std::min(observed.size(), kMutationMagic.size());
      if (!kMutationMagic.starts_with(std::string_view(partial.data(), prefix))) {
        return std::unexpected(Corrupt("invalid partial mutation magic"));
      }
      break;
    }
    auto frame = ReadFrameHeader(log, replay.offset, Kind::Mutation);
    if (!frame) {
      return std::unexpected(std::move(frame).error());
    }
    if (frame->sequence != replay.frontier + 1) {
      return std::unexpected(Corrupt("nonconsecutive mutation sequence"));
    }
    const size_t length = kFrameHeaderBytes + frame->payload_bytes;
    if (length > remaining) {
      // Validate that all bytes in the captured extent are still readable before trimming.
      scratch.resize(static_cast<size_t>(remaining) - kFrameHeaderBytes);
      if (auto status = ReadExact(log, replay.offset + kFrameHeaderBytes, scratch); !status.ok()) {
        return std::unexpected(std::move(status));
      }
      break;
    }
    auto ops = ReadPayload(log, replay.offset, *frame, Kind::Mutation, scratch);
    if (!ops) {
      return std::unexpected(std::move(ops).error());
    }
    for (size_t ordinal = 0; ordinal < ops->size(); ++ordinal) {
      Apply(replay.index, (*ops)[ordinal], {Kind::Mutation, replay.offset, length, ordinal});
    }
    replay.frontier = frame->sequence;
    replay.offset += length;
    ++replay.records;
  }
  return replay;
}

} // namespace

struct DB::Impl {
  enum class State : uint8_t { Healthy, ReopenRequired, Closed };

  Impl(File lock_file, File log_file, Replay&& replay)
      : lock(std::move(lock_file)), log(std::move(log_file)), header(replay.header),
        index(std::move(replay.index)), frontier(replay.frontier), offset(replay.offset) {}

  Status CheckHealthy() const noexcept try {
    if (state == State::Healthy) {
      return Status::OK();
    }
    return Status::IOError(state == State::Closed ? "DB closed" : "DB requires reopen");
  } catch (...) {
    return Status::FromCode(Code::kIOError);
  }

  Result<std::string> ReadValue(const Entry& entry, std::string_view key) {
    auto frame = ReadFrameHeader(log, entry.offset, entry.kind);
    if (!frame) {
      return std::unexpected(std::move(frame).error());
    }
    if (kFrameHeaderBytes + frame->payload_bytes != entry.bytes || entry.ordinal >= frame->count) {
      return std::unexpected(Corrupt("indexed frame extent or ordinal mismatch"));
    }
    std::string scratch;
    auto ops = ReadPayload(log, entry.offset, *frame, entry.kind, scratch);
    if (!ops) {
      return std::unexpected(std::move(ops).error());
    }
    const auto& op = (*ops)[entry.ordinal];
    if (op.tag != Tag::Put || op.key != key) {
      return std::unexpected(Corrupt("indexed operation mismatch"));
    }
    return std::string(op.value);
  }

  Status Commit(std::span<const Operation> ops) noexcept try {
    std::lock_guard guard(mutex);
    LAMINAR_RETURN_IF_ERROR(CheckHealthy());
    if (ops.empty()) {
      return Status::OK();
    }
    if (frontier == kMaxSequence) {
      return Status::Full("maximum commit sequence");
    }
    const uint64_t sequence = frontier + 1;
    auto bytes = EncodeFrame(Kind::Mutation, sequence, ops);
    if (!bytes) {
      return std::move(bytes).error();
    }
    if (bytes->size() > kMaxOffset - offset) {
      return Status::Full("maximum log offset");
    }

    state = State::ReopenRequired;
    if (auto status = log.Append(*bytes); !status.ok()) {
      return ReopenRequired(status);
    }
    if (auto status = log.Sync(Point::DataSync); !status.ok()) {
      return ReopenRequired(status);
    }
    try {
      At(Point::BeforePublish, kLog);
      for (size_t ordinal = 0; ordinal < ops.size(); ++ordinal) {
        Apply(index, ops[ordinal], {Kind::Mutation, offset, bytes->size(), ordinal});
        At(Point::AfterApply, kLog);
      }
    } catch (...) {
      return ReopenRequired(CurrentException());
    }
    frontier = sequence;
    offset += bytes->size();
    state = State::Healthy;
    At(Point::AfterPublish, kLog);
    return Status::OK();
  } catch (...) {
    return CurrentException();
  }

  std::mutex mutex;
  File lock;
  File log;
  FileHeader header;
  Index index;
  uint64_t frontier;
  uint64_t offset;
  State state{State::Healthy};
};

Status WriteBatch::Put(Slice key, Slice value) noexcept { return Add(key, value, false); }

Status WriteBatch::Delete(Slice key) noexcept { return Add(key, {}, true); }

Status WriteBatch::Add(Slice key, Slice value, bool erase) noexcept try {
  if (key.size() > kMaxKey || value.size() > kMaxValue) {
    return Status::InvalidArgument("batch key/value limit");
  }
  if (mutations_.size() == kMaxOperations) {
    return Status::InvalidArgument("batch operation limit");
  }
  const size_t payload = mutations_.empty() ? 0 : mutations_.back().payload_end;
  const size_t size = kOperationBytes + key.size() + value.size();
  if (size > kMaxPayload - payload) {
    return Status::InvalidArgument("batch payload limit");
  }
  mutations_.push_back({erase, key.ToString(), value.ToString(), payload + size});
  return Status::OK();
} catch (...) {
  return CurrentException();
}

DB::DB(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DB::~DB() = default;

Result<std::unique_ptr<DB>> DB::Open(std::string_view directory) noexcept try {
  auto locked = LockDirectory(directory, true);
  if (!locked) {
    return std::unexpected(std::move(locked).error());
  }
  const auto open_log = [&] {
    return File::OpenAt(locked->dir.fd(), kLog, kLog, O_RDWR | O_APPEND | O_NOFOLLOW);
  };
  auto log = open_log();
  if (!log && log.error().IsNotFound()) {
    if (auto status = CreateLog(locked->dir.fd()); !status.ok()) {
      return std::unexpected(std::move(status));
    }
    auto created = open_log();
    if (!created) {
      return std::unexpected(std::move(created).error());
    }
    log.emplace(std::move(*created));
  }
  if (!log) {
    return std::unexpected(std::move(log).error());
  }
  auto replay = Scan(*log);
  if (!replay) {
    return std::unexpected(std::move(replay).error());
  }
  if (replay->offset != replay->size) {
    if (auto status = log->Truncate(replay->offset); !status.ok()) {
      return std::unexpected(std::move(status));
    }
  }
  if (auto status = log->Sync(Point::DataSync); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = locked->dir.Sync(Point::Sync); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = locked->parent.Sync(Point::Sync); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = locked->dir.Close(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = locked->parent.Close(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return std::unique_ptr<DB>(
      new DB(std::make_unique<Impl>(std::move(locked->lock), std::move(*log), std::move(*replay))));
} catch (...) {
  return std::unexpected(CurrentException());
}

Result<Inspection> Inspect(std::string_view directory) noexcept try {
  auto locked = LockDirectory(directory, false);
  if (!locked) {
    return std::unexpected(std::move(locked).error());
  }
  auto log = File::OpenAt(locked->dir.fd(), kLog, kLog, O_RDONLY | O_NOFOLLOW);
  if (!log) {
    return std::unexpected(std::move(log).error());
  }
  auto replay = Scan(*log);
  if (!replay) {
    return std::unexpected(std::move(replay).error());
  }
  if (auto status = log->Close(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (auto status = locked->lock.Close(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return Inspection{1, replay->size, replay->records, replay->index.size(),
                    replay->size - replay->offset};
} catch (...) {
  return std::unexpected(CurrentException());
}

Status DB::Put(Slice key, Slice value) noexcept {
  const Operation op{Tag::Put, key.ToStringView(), value.ToStringView()};
  return impl_->Commit(std::span(&op, 1));
}

Status DB::Delete(Slice key) noexcept {
  const Operation op{Tag::Delete, key.ToStringView(), {}};
  return impl_->Commit(std::span(&op, 1));
}

Status DB::Write(const WriteBatch& batch) noexcept {
  std::array<Operation, kMaxOperations> ops;
  size_t count = 0;
  for (const auto& mutation : batch.mutations_) {
    ops[count++] = {mutation.erase ? Tag::Delete : Tag::Put, mutation.key, mutation.value};
  }
  return impl_->Commit(std::span(ops).first(count));
}

Result<std::string> DB::Get(Slice key) noexcept try {
  std::lock_guard guard(impl_->mutex);
  if (auto status = impl_->CheckHealthy(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (key.size() > kMaxKey) {
    return std::unexpected(Status::InvalidArgument("key limit"));
  }
  const auto view = key.ToStringView();
  const auto found = impl_->index.find(view);
  if (found == impl_->index.end()) {
    return std::unexpected(Status::FromCode(Code::kNotFound));
  }
  auto value = impl_->ReadValue(found->second, view);
  if (!value) {
    impl_->state = Impl::State::ReopenRequired;
  }
  return value;
} catch (...) {
  return std::unexpected(CurrentException());
}

Status DB::Close() noexcept try {
  std::lock_guard guard(impl_->mutex);
  impl_->state = Impl::State::Closed;
  auto log_status = impl_->log.Close();
  auto lock_status = impl_->lock.Close();
  impl_->index.clear();
  return log_status.ok() ? std::move(lock_status) : std::move(log_status);
} catch (...) {
  return CurrentException();
}

} // namespace laminar
