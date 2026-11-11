#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "laminar/db.h"

namespace laminar {
namespace projection {
struct TestAccess;
}

// One owning process and exclusively owned standalone Redis namespace, without eviction/TTL.
// All application projection reads must use this handle. Join callers before destruction.
class RedisProjection {
public:
  [[nodiscard]] static Result<std::unique_ptr<RedisProjection>>
  Open(std::string_view socket_path, std::string_view name) noexcept;
  ~RedisProjection();
  RedisProjection(const RedisProjection&) = delete;
  RedisProjection& operator=(const RedisProjection&) = delete;

  // Only feed whole batches returned by this source's ReadChangesAfter.
  [[nodiscard]] Status Apply(const CommittedBatch& batch) noexcept;
  [[nodiscard]] Result<std::string> Get(Slice key) noexcept;
  // Unready after every owner restart or unresolved execution error.
  [[nodiscard]] Result<ChangeCursor> Cursor() noexcept;

private:
  friend struct projection::TestAccess;
  struct Impl;
  explicit RedisProjection(std::unique_ptr<Impl> impl);
  Status PublishReady(ChangeCursor cursor);
  std::unique_ptr<Impl> impl_;
};
} // namespace laminar
