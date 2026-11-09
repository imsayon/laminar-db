#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

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

class DB {
public:
  [[nodiscard]] static Result<std::unique_ptr<DB>> Open(std::string_view directory) noexcept;
  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  [[nodiscard]] Status Put(Slice key, Slice value) noexcept;
  [[nodiscard]] Result<std::string> Get(Slice key) noexcept;
  [[nodiscard]] Status Delete(Slice key) noexcept;
  [[nodiscard]] Status Close() noexcept;

private:
  struct Impl;
  explicit DB(std::unique_ptr<Impl> impl);
  Status Mutate(Slice key, Slice value, bool erase) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace laminar
