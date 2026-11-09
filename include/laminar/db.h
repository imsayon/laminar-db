#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "laminar/slice.h"
#include "laminar/status.h"

namespace laminar {

struct Inspection {
  uint32_t format_version;
  uint64_t log_bytes;
  uint64_t record_count;
  uint64_t live_keys;
  uint64_t incomplete_tail_bytes;
};

[[nodiscard]] Result<Inspection> Inspect(std::string_view directory) noexcept;

class WriteBatch {
public:
  WriteBatch() = default;
  WriteBatch(WriteBatch&&) noexcept = default;
  WriteBatch& operator=(WriteBatch&&) noexcept = default;

  [[nodiscard]] Status Put(Slice key, Slice value) noexcept;
  [[nodiscard]] Status Delete(Slice key) noexcept;
  void Clear() noexcept { mutations_.clear(); }

private:
  friend class DB;

  struct Mutation {
    bool erase;
    std::string key;
    std::string value;
    size_t payload_end;
  };

  Status Add(Slice key, Slice value, bool erase) noexcept;
  std::vector<Mutation> mutations_;
};

// Operations and Close may share a live handle. Join callers before destruction.
class DB {
public:
  [[nodiscard]] static Result<std::unique_ptr<DB>> Open(std::string_view directory) noexcept;
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  [[nodiscard]] Status Put(Slice key, Slice value) noexcept;
  [[nodiscard]] Result<std::string> Get(Slice key) noexcept;
  [[nodiscard]] Status Delete(Slice key) noexcept;
  // Publish in batch order after syncing. An error can follow a durable write.
  // The caller must keep the batch alive and unchanged until Write returns.
  [[nodiscard]] Status Write(const WriteBatch& batch) noexcept;
  [[nodiscard]] Status Close() noexcept;

private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace laminar
