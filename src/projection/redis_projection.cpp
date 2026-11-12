#include "laminar/redis_projection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>

#include "projection/redis.h"
#include "storage/format.h"
#include "storage/maintenance.h"

namespace laminar {
namespace {
using namespace projection;
using namespace storage;

Status Exception() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return Status::FromCode(Status::Code::kFull);
  } catch (const std::length_error&) {
    return Status::FromCode(Status::Code::kFull);
  } catch (...) {
    return Status::FromCode(Status::Code::kIOError);
  }
}

constexpr std::string_view kMarker = R"lua(
local id = redis.call('HGET', KEYS[1], 'id')
local seq = redis.call('HGET', KEYS[1], 'seq')
local ready = redis.call('HGET', KEYS[1], 'ready')
if not id or not seq or not ready then return '' end
return id .. seq .. ready
)lua";

constexpr std::string_view kApply = R"lua(
local function hex(s, n)
  return #s == n and string.match(s, '^[0-9a-f]+$') ~= nil
end
local function successor(s)
  local digits = '0123456789abcdef'
  for i=16,1,-1 do
    local n = string.find(digits, string.sub(s,i,i), 1, true)
    if n < 16 then return string.sub(s,1,i-1) .. string.sub(digits,n+1,n+1) .. string.rep('0',16-i) end
  end
  return ''
end
if #KEYS ~= 2 or #ARGV < 6 or not hex(ARGV[1],32) or not hex(ARGV[2],16) or
   not hex(ARGV[3],16) or ARGV[3] ~= successor(ARGV[2]) then
  return redis.error_reply('invalid identity/sequence')
end
local count = (#ARGV - 3) / 3
if count < 1 or count > 1024 or count ~= math.floor(count) then
  return redis.error_reply('operation count')
end
local bytes = 0
for n=1,count do
  local i = 3*n+1
  local tag, key, value = ARGV[i], ARGV[i+1], ARGV[i+2]
  if (tag ~= '1' and tag ~= '2') or #key > 65536 or #value > 8388608 or
     (tag == '2' and #value ~= 0) then return redis.error_reply('invalid operation') end
  bytes = bytes + 9 + #key + #value
  if bytes > 16777216 then return redis.error_reply('payload limit') end
end
if redis.call('HGET',KEYS[2],'id') ~= ARGV[1] or
   redis.call('HGET',KEYS[2],'seq') ~= ARGV[2] or
   redis.call('HGET',KEYS[2],'ready') ~= '1' then return redis.error_reply('epoch/gap') end
redis.call('HSET',KEYS[2],'ready','0')
for n=1,count do
  local i = 3*n+1
  if ARGV[i] == '1' then redis.call('HSET',KEYS[1],ARGV[i+1],ARGV[i+2])
  else redis.call('HDEL',KEYS[1],ARGV[i+1]) end
  -- test failure point
end
redis.call('HSET',KEYS[2],'seq',ARGV[3],'ready','1')
return 'applied'
)lua";

Status Validate(const CommittedBatch& batch) {
  if (batch.operations.empty() || batch.operations.size() > kMaxOperations ||
      batch.cursor.sequence == 0 ||
      std::ranges::all_of(batch.cursor.db_id, [](char byte) { return byte == 0; })) {
    return Status::InvalidArgument("invalid committed batch identity/count/sequence");
  }
  size_t bytes = kFrameHeaderBytes;
  for (const auto& op : batch.operations) {
    if ((op.type != Change::Type::Put && op.type != Change::Type::Delete) ||
        op.key.size() > kMaxKey || op.value.size() > kMaxValue ||
        (op.type == Change::Type::Delete && !op.value.empty())) {
      return Status::InvalidArgument("invalid projected operation");
    }
    const size_t size = kOperationBytes + op.key.size() + op.value.size();
    if (size > kMaxPayload + kFrameHeaderBytes - bytes) {
      return Status::InvalidArgument("projected payload limit");
    }
    bytes += size;
  }
  return bytes == batch.encoded_bytes ? Status::OK()
                                      : Status::InvalidArgument("projected frame size mismatch");
}
} // namespace

struct RedisProjection::Impl {
  Impl(std::string_view path, std::string_view name)
      : socket(path), data("laminar:" + std::string(name) + ":data"),
        meta("laminar:" + std::string(name) + ":meta") {}

  Result<std::optional<std::string>> Run(std::initializer_list<std::string_view> args) const {
    return Command(socket, std::span(args.begin(), args.size()));
  }

  Result<std::string> Marker() const {
    const auto result = Run({"EVAL", kMarker, "1", meta});
    if (!result) {
      return std::unexpected(result.error());
    }
    return result->value_or("");
  }

  std::string Expected(ChangeCursor position) const {
    return IdentityHex(position.db_id) + SequenceHex(position.sequence) + "1";
  }

  Status CheckReady() {
    if (!ready) {
      return Status::IOError("projection unready; reconcile/rebuild required");
    }
    const auto marker = Marker();
    if (!marker || *marker != Expected(cursor)) {
      ready = false;
      return marker ? Status::IOError("projection epoch lost; rebuild required") : marker.error();
    }
    return Status::OK();
  }

  Status Compare(Maintenance& source) const {
    const auto count = Run({"HLEN", data});
    if (!count) {
      return count.error();
    }
    if (*count != std::optional<std::string>(std::to_string(source.Size()))) {
      return Status::IOError("projection cardinality mismatch; rebuild required");
    }
    source.Rewind();
    for (;;) {
      auto row = source.Next();
      if (!row) {
        return std::move(row).error();
      }
      if (!*row) {
        break;
      }
      const auto& [key, value] = **row;
      const auto actual = Run({"HGET", data, key});
      if (!actual) {
        return actual.error();
      }
      if (!*actual || **actual != value) {
        return Status::IOError("projection key/value mismatch; rebuild required");
      }
    }
    return Status::OK();
  }

  std::mutex mutex;
  std::string socket, data, meta;
  ChangeCursor cursor;
  bool ready{false};
};

RedisProjection::RedisProjection(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RedisProjection::~RedisProjection() = default;

Result<std::unique_ptr<RedisProjection>> RedisProjection::Open(std::string_view socket_path,
                                                               std::string_view name) noexcept try {
  if (socket_path.empty() || socket_path.size() >= 108 ||
      socket_path.find('\0') != std::string_view::npos || name.empty() || name.size() > 64 ||
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") !=
          std::string_view::npos) {
    return std::unexpected(Status::InvalidArgument("invalid projection socket/name"));
  }
  return std::unique_ptr<RedisProjection>(
      new RedisProjection(std::make_unique<Impl>(socket_path, name)));
} catch (...) {
  return std::unexpected(Exception());
}

Status RedisProjection::PublishReady(ChangeCursor cursor) {
  const auto id = IdentityHex(cursor.db_id), seq = SequenceHex(cursor.sequence);
  const auto result = impl_->Run({"HSET", impl_->meta, "id", id, "seq", seq, "ready", "1"});
  if (!result) {
    return result.error();
  }
  const auto marker = impl_->Marker();
  if (!marker || *marker != impl_->Expected(cursor)) {
    return marker ? Status::IOError("projection ready verification failed") : marker.error();
  }
  impl_->cursor = cursor;
  impl_->ready = true;
  return Status::OK();
}

Status RedisProjection::Reconcile(DB& source) noexcept try {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  auto stable = Maintenance::Pause(source);
  if (!stable) {
    return std::move(stable).error();
  }
  const auto cursor = (*stable)->Cursor();
  const auto marker = impl_->Marker();
  if (!marker || *marker != impl_->Expected(cursor)) {
    return marker ? Status::IOError("projection checkpoint mismatch; rebuild required")
                  : marker.error();
  }
  LAMINAR_RETURN_IF_ERROR(impl_->Compare(**stable));
  const auto verified = impl_->Marker();
  if (!verified || *verified != impl_->Expected(cursor)) {
    return verified ? Status::IOError("projection epoch changed during comparison")
                    : verified.error();
  }
  impl_->cursor = cursor;
  impl_->ready = true;
  return Status::OK();
} catch (...) {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  return Exception();
}

Status RedisProjection::Rebuild(DB& source) noexcept try {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  auto stable = Maintenance::Pause(source);
  if (!stable) {
    return std::move(stable).error();
  }
  const auto cursor = (*stable)->Cursor();
  const auto cleared = impl_->Run({"DEL", impl_->data, impl_->meta});
  if (!cleared) {
    return cleared.error();
  }
  const auto closed = impl_->Run({"HSET", impl_->meta, "ready", "0"});
  if (!closed) {
    return closed.error();
  }
  for (;;) {
    auto row = (*stable)->Next();
    if (!row) {
      return std::move(row).error();
    }
    if (!*row) {
      break;
    }
    const auto& [key, value] = **row;
    const auto result = impl_->Run({"HSET", impl_->data, key, value});
    if (!result) {
      return result.error();
    }
    if (At(projection::Point::RebuildRow).script_error) {
      return Status::IOError("injected interrupted projection rebuild");
    }
  }
  LAMINAR_RETURN_IF_ERROR(impl_->Compare(**stable));
  if (At(projection::Point::BeforeReady).script_error) {
    return Status::IOError("injected failure before projection publication");
  }
  return PublishReady(cursor);
} catch (...) {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  return Exception();
}

Status RedisProjection::Apply(const CommittedBatch& batch) noexcept try {
  std::lock_guard guard(impl_->mutex);
  LAMINAR_RETURN_IF_ERROR(Validate(batch));
  LAMINAR_RETURN_IF_ERROR(impl_->CheckReady());
  if (batch.cursor.db_id != impl_->cursor.db_id) {
    return Status::InvalidArgument("foreign projection batch");
  }
  if (batch.cursor.sequence <= impl_->cursor.sequence) {
    return Status::OK();
  }
  if (impl_->cursor.sequence == std::numeric_limits<uint64_t>::max() ||
      batch.cursor.sequence != impl_->cursor.sequence + 1) {
    return Status::InvalidArgument("projection gap");
  }
  std::string script(kApply);
  if (At(projection::Point::Script).script_error) {
    const auto point = script.find("-- test failure point");
    script.replace(point, std::string_view("-- test failure point").size(),
                   "if n == 1 then redis.call('HSET',KEYS[1]) end");
  }
  const auto id = IdentityHex(batch.cursor.db_id);
  const auto previous = SequenceHex(impl_->cursor.sequence);
  const auto next = SequenceHex(batch.cursor.sequence);
  std::vector<std::string_view> args{"EVAL",      script, "2",      impl_->data,
                                     impl_->meta, id,     previous, next};
  args.reserve(args.size() + batch.operations.size() * 3);
  for (const auto& op : batch.operations) {
    args.push_back(op.type == Change::Type::Put ? "1" : "2");
    args.push_back(op.key);
    args.push_back(op.value);
  }
  // The mutex drains existing reads before dispatch; no reader enters until verified success.
  impl_->ready = false;
  Status error = Status::IOError("projection apply unresolved");
  for (unsigned attempt = 0; attempt <= 3; ++attempt) {
    const auto marker = impl_->Marker();
    if (!marker) {
      return marker.error();
    }
    if (attempt > 0 && *marker == impl_->Expected(batch.cursor)) {
      impl_->cursor = batch.cursor;
      impl_->ready = true;
      return Status::OK();
    }
    if (*marker != impl_->Expected(impl_->cursor) || attempt == 3) {
      return error;
    }
    const auto result = Command(impl_->socket, args);
    if (result && *result == std::optional<std::string>("applied")) {
      impl_->cursor = batch.cursor;
      impl_->ready = true;
      return Status::OK();
    }
    error = result ? Status::IOError("unexpected Redis apply reply") : result.error();
    std::this_thread::sleep_for(std::chrono::milliseconds(10U << attempt));
  }
  return error;
} catch (...) {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  return Exception();
}

Result<std::string> RedisProjection::Get(Slice key) noexcept try {
  std::lock_guard guard(impl_->mutex);
  if (key.size() > kMaxKey) {
    return std::unexpected(Status::InvalidArgument("projection key limit"));
  }
  if (auto status = impl_->CheckReady(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  const auto result = impl_->Run({"HGET", impl_->data, key.ToStringView()});
  if (!result) {
    impl_->ready = false;
    return std::unexpected(result.error());
  }
  if (!*result) {
    return std::unexpected(Status::NotFound());
  }
  return **result;
} catch (...) {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  return std::unexpected(Exception());
}

Result<ChangeCursor> RedisProjection::Cursor() noexcept try {
  std::lock_guard guard(impl_->mutex);
  if (auto status = impl_->CheckReady(); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  return impl_->cursor;
} catch (...) {
  std::lock_guard guard(impl_->mutex);
  impl_->ready = false;
  return std::unexpected(Exception());
}
} // namespace laminar
