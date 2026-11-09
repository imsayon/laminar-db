#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <sys/types.h>

#include "laminar/status.h"

namespace laminar::storage {

enum class Point : uint8_t {
  Open,
  Stat,
  Mkdir,
  Lock,
  Read,
  Write,
  DataSync,
  Sync,
  Truncate,
  Unlink,
  Rename,
  Close,
  Random,
  BeforePublish,
  AfterApply,
  AfterPublish,
};

struct Fault {
  int error{};
  size_t limit{std::numeric_limits<size_t>::max()};
};

// Test-only seam. A transfer limit bounds the real syscall, so bytes reach the real filesystem.
using FaultHook = Fault (*)(Point, std::string_view) noexcept;
extern thread_local FaultHook fault_hook;
Fault At(Point point, std::string_view target) noexcept;

inline constexpr uint64_t kMaxOffset = static_cast<uint64_t>(std::numeric_limits<off_t>::max());

class File {
public:
  File(File&& other) noexcept;
  ~File();
  File& operator=(File&&) = delete;

  static Result<File> OpenAt(int directory, const char* path, const char* label, int flags,
                             Status::Code type_error = Status::Code::kIOError);
  [[nodiscard]] int fd() const { return fd_; }

  Status Close() noexcept;
  Status Lock() const;
  Result<uint64_t> Size() const;
  Result<size_t> Read(uint64_t offset, std::span<char> bytes) const;
  Status Append(std::string_view bytes) const;
  Status Truncate(uint64_t size) const;
  Status Sync(Point point) const;

private:
  File(int fd, const char* label) : fd_(fd), label_(label) {}
  int fd_;
  const char* label_;
};

Status MakeDirectory(int parent, const char* name);
Status RemoveFile(int directory, const char* name);
Status InstallFile(int directory, const char* temp, const char* destination);
Status RandomBytes(std::span<char> bytes);

} // namespace laminar::storage
