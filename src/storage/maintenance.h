#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "laminar/db.h"

namespace laminar::storage {
// Internal paused-write iterator. The source handle must outlive this guard.
class Maintenance {
public:
  static Result<std::unique_ptr<Maintenance>> Pause(DB& db) noexcept;
  ~Maintenance();
  Maintenance(const Maintenance&) = delete;
  Maintenance& operator=(const Maintenance&) = delete;
  ChangeCursor Cursor() const;
  size_t Size() const;
  void Rewind();
  Result<std::optional<std::pair<std::string, std::string>>> Next() noexcept;

private:
  struct Impl;
  explicit Maintenance(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
} // namespace laminar::storage
