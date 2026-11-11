#include "storage/file.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace laminar::storage {

static_assert(sizeof(off_t) == 8 && std::numeric_limits<off_t>::is_signed);

thread_local FaultHook fault_hook = nullptr;

Fault At(Point point, std::string_view target) noexcept {
  return fault_hook == nullptr ? Fault{} : fault_hook(point, target);
}

namespace {

Status FileError(const char* operation, const char* target, int error,
                 Status::Code code = Status::Code::kIOError) noexcept {
  try {
    const auto message = std::string(operation) + " " + target + ": " +
                         std::error_code(error, std::generic_category()).message() + " (errno " +
                         std::to_string(error) + ")";
    switch (code) {
    case Status::Code::kNotFound:
      return Status::NotFound(message);
    case Status::Code::kAlreadyExists:
      return Status::AlreadyExists(message);
    default:
      return Status::IOError(message);
    }
  } catch (...) {
    return Status::FromCode(code);
  }
}

template <typename Call> auto Retry(Point point, const char* label, Call call) {
  decltype(call(Fault{})) result;
  do {
    const auto fault = At(point, label);
    if (fault.error != 0) {
      errno = fault.error;
      result = -1;
    } else {
      result = call(fault);
    }
  } while (result < 0 && errno == EINTR);
  return result;
}

} // namespace

File::File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)), label_(other.label_) {}

File::~File() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Result<File> File::OpenAt(int directory, const char* path, const char* label, int flags,
                          Status::Code type_error) {
  const int fd = Retry(Point::Open, label, [&](Fault) {
    return ::openat(directory, path, flags | O_CLOEXEC | O_NONBLOCK, 0600);
  });
  if (fd < 0) {
    const int error = errno;
    auto code = Status::Code::kIOError;
    if (error == ENOENT) {
      code = Status::Code::kNotFound;
    } else if (error == ELOOP) {
      code = type_error;
    } else if (error == EEXIST) {
      code = Status::Code::kAlreadyExists;
    }
    return std::unexpected(FileError("open", path, error, code));
  }
  File file(fd, label);
  struct stat info = {};
  if (Retry(Point::Stat, label, [&](Fault) { return ::fstat(fd, &info); }) < 0) {
    return std::unexpected(FileError("stat", path, errno));
  }
  if ((flags & O_DIRECTORY) != 0 ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode)) {
    return std::unexpected(FileError("unexpected file type", path, EINVAL, type_error));
  }
  return file;
}

Status File::Close() noexcept {
  if (fd_ < 0) {
    return Status::OK();
  }
  // Linux consumes the descriptor even when close reports an error.
  const int result = ::close(std::exchange(fd_, -1));
  const int error = result < 0 ? errno : 0;
  const int injected = At(Point::Close, label_).error;
  if (error != 0 || injected != 0) {
    return FileError("close", label_, error != 0 ? error : injected);
  }
  return Status::OK();
}

Status File::Lock() const {
  if (Retry(Point::Lock, label_, [&](Fault) { return ::flock(fd_, LOCK_EX | LOCK_NB); }) < 0) {
    const int error = errno;
    return FileError("flock", label_, error,
                     error == EWOULDBLOCK ? Status::Code::kAlreadyExists : Status::Code::kIOError);
  }
  return Status::OK();
}

Result<uint64_t> File::Size() const {
  struct stat info = {};
  if (Retry(Point::Stat, label_, [&](Fault) { return ::fstat(fd_, &info); }) < 0) {
    return std::unexpected(FileError("stat", label_, errno));
  }
  if (info.st_size < 0) {
    return std::unexpected(FileError("negative file size", label_, EOVERFLOW));
  }
  return static_cast<uint64_t>(info.st_size);
}

Result<size_t> File::Read(uint64_t offset, std::span<char> bytes) const {
  if (offset > kMaxOffset || bytes.size() > kMaxOffset - offset) {
    return std::unexpected(FileError("pread extent", label_, EOVERFLOW));
  }
  size_t done = 0;
  while (done < bytes.size()) {
    const auto count = Retry(Point::Read, label_, [&](Fault fault) {
      return ::pread(fd_, bytes.data() + done, std::min(bytes.size() - done, fault.limit),
                     static_cast<off_t>(offset + done));
    });
    if (count < 0) {
      return std::unexpected(FileError("pread", label_, errno));
    }
    if (count == 0) {
      break;
    }
    done += static_cast<size_t>(count);
  }
  return done;
}

Status File::Append(std::string_view bytes) const {
  while (!bytes.empty()) {
    const auto count = Retry(Point::Write, label_, [&](Fault fault) {
      return ::write(fd_, bytes.data(), std::min(bytes.size(), fault.limit));
    });
    if (count <= 0) {
      return FileError("write", label_, count < 0 ? errno : EIO);
    }
    bytes.remove_prefix(static_cast<size_t>(count));
  }
  return Status::OK();
}

Status File::WriteAt(uint64_t offset, std::string_view bytes) const {
  if (offset > kMaxOffset || bytes.size() > kMaxOffset - offset) {
    return FileError("pwrite extent", label_, EOVERFLOW);
  }
  while (!bytes.empty()) {
    const auto count = Retry(Point::PositionedWrite, label_, [&](Fault fault) {
      return ::pwrite(fd_, bytes.data(), std::min(bytes.size(), fault.limit),
                      static_cast<off_t>(offset));
    });
    if (count <= 0) {
      return FileError("pwrite", label_, count < 0 ? errno : EIO);
    }
    offset += static_cast<size_t>(count);
    bytes.remove_prefix(static_cast<size_t>(count));
  }
  return Status::OK();
}

Status File::Truncate(uint64_t size) const {
  if (size > kMaxOffset) {
    return FileError("truncate extent", label_, EOVERFLOW);
  }
  if (Retry(Point::Truncate, label_,
            [&](Fault) { return ::ftruncate(fd_, static_cast<off_t>(size)); }) < 0) {
    return FileError("truncate", label_, errno);
  }
  return Status::OK();
}

Status File::Sync(Point point) const {
  if (Retry(point, label_, [&](Fault) {
        return point == Point::DataSync ? ::fdatasync(fd_) : ::fsync(fd_);
      }) < 0) {
    return FileError(point == Point::DataSync ? "fdatasync" : "fsync", label_, errno);
  }
  return Status::OK();
}

Status MakeDirectory(int parent, const char* name) {
  if (Retry(Point::Mkdir, "DB directory", [&](Fault) { return ::mkdirat(parent, name, 0700); }) <
      0) {
    const int error = errno;
    if (error != EEXIST) {
      return FileError("mkdir", name, error);
    }
  }
  return Status::OK();
}

Status RemoveFile(int directory, const char* name) {
  if (Retry(Point::Unlink, name, [&](Fault) { return ::unlinkat(directory, name, 0); }) < 0) {
    return FileError("unlink", name, errno);
  }
  return Status::OK();
}

Status InstallFile(int directory, const char* temp, const char* destination) {
  if (Retry(Point::Rename, temp, [&](Fault) {
        return ::renameat2(directory, temp, directory, destination, RENAME_NOREPLACE);
      }) < 0) {
    const int error = errno;
    return FileError("install as", destination, error,
                     error == EEXIST ? Status::Code::kAlreadyExists : Status::Code::kIOError);
  }
  return Status::OK();
}

Status ReplaceFile(int directory, const char* temp, const char* destination) {
  if (Retry(Point::Rename, temp,
            [&](Fault) { return ::renameat(directory, temp, directory, destination); }) < 0) {
    return FileError("replace", destination, errno);
  }
  return Status::OK();
}

Status RandomBytes(std::span<char> bytes) {
  while (!bytes.empty()) {
    const auto count = Retry(Point::Random, "db_id", [&](Fault fault) {
      return ::getrandom(bytes.data(), std::min(bytes.size(), fault.limit), 0);
    });
    if (count <= 0) {
      return FileError("getrandom", "db_id", count < 0 ? errno : EIO);
    }
    bytes = bytes.subspan(static_cast<size_t>(count));
  }
  return Status::OK();
}

} // namespace laminar::storage
