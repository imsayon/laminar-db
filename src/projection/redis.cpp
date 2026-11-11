#include "projection/redis.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "storage/format.h"

namespace laminar::projection {
thread_local FaultHook fault_hook = nullptr;
Fault At(Point point) noexcept { return fault_hook == nullptr ? Fault{} : fault_hook(point); }

std::string SequenceHex(uint64_t sequence) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result(16, '0');
  for (size_t i = 16; i > 0; --i) {
    result[i - 1] = digits[sequence & 15U];
    sequence >>= 4;
  }
  return result;
}

std::string IdentityHex(const std::array<char, 16>& id) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  for (const char byte : id) {
    const auto value = static_cast<unsigned char>(byte);
    result += digits[value >> 4];
    result += digits[value & 15U];
  }
  return result;
}

namespace {
using Clock = std::chrono::steady_clock;
struct Socket {
  int fd;
  Clock::time_point deadline{Clock::now() + std::chrono::seconds(2)};
  ~Socket() {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  Status Wait(short events) const {
    for (;;) {
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
      if (left.count() <= 0) {
        return Status::IOError("Redis command deadline exceeded");
      }
      pollfd item{fd, events, 0};
      const int result = ::poll(&item, 1, static_cast<int>(left.count()));
      if (result > 0) {
        return Status::OK();
      }
      if (result < 0 && errno != EINTR) {
        return Status::IOError("Redis socket poll failed");
      }
    }
  }

  Status Transfer(char* data, size_t size, bool send) const {
    while (size > 0) {
      LAMINAR_RETURN_IF_ERROR(Wait(send ? POLLOUT : POLLIN));
      const auto count = send ? ::send(fd, data, size, MSG_NOSIGNAL) : ::recv(fd, data, size, 0);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      if (count <= 0) {
        return Status::IOError("Redis connection lost");
      }
      data += count;
      size -= static_cast<size_t>(count);
    }
    return Status::OK();
  }

  Result<std::string> Line() const {
    std::string line;
    while (line.size() < 4096) {
      char byte{};
      if (auto status = Transfer(&byte, 1, false); !status.ok()) {
        return std::unexpected(std::move(status));
      }
      line += byte;
      if (line.ends_with("\r\n")) {
        line.resize(line.size() - 2);
        return line;
      }
    }
    return std::unexpected(Status::IOError("oversized Redis reply header"));
  }
};
} // namespace

Result<std::optional<std::string>> Command(std::string_view socket_path,
                                           std::span<const std::string_view> args) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.empty() || socket_path.size() >= sizeof(address.sun_path) ||
      socket_path.find('\0') != std::string_view::npos || args.empty()) {
    return std::unexpected(Status::InvalidArgument("invalid Redis socket/command"));
  }
  std::memcpy(address.sun_path, socket_path.data(), socket_path.size());
  std::string request = "*" + std::to_string(args.size()) + "\r\n";
  constexpr size_t kMaxRequest = storage::kMaxPayload + 262144;
  for (const auto arg : args) {
    const auto header = "$" + std::to_string(arg.size()) + "\r\n";
    if (header.size() + 2 > kMaxRequest - request.size() ||
        arg.size() > kMaxRequest - request.size() - header.size() - 2) {
      return std::unexpected(Status::InvalidArgument("Redis request limit"));
    }
    request += header;
    request += arg;
    request += "\r\n";
  }
  const Socket socket{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
  if (socket.fd < 0) {
    return std::unexpected(Status::IOError("Redis socket failed"));
  }
  if (::connect(socket.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
    if (errno != EINPROGRESS && errno != EAGAIN) {
      return std::unexpected(Status::IOError("Redis connect failed"));
    }
    if (auto status = socket.Wait(POLLOUT); !status.ok()) {
      return std::unexpected(std::move(status));
    }
    int error = 0;
    socklen_t size = sizeof(error);
    if (::getsockopt(socket.fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
      return std::unexpected(Status::IOError("Redis connect failed"));
    }
  }
  if (auto status = socket.Transfer(request.data(), request.size(), true); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (args.size() > 2 && args[0] == "EVAL" && args[2] == "2" && At(Point::Sent).lose_reply) {
    return std::unexpected(Status::IOError("injected lost Redis reply"));
  }
  auto line = socket.Line();
  if (!line) {
    return std::unexpected(std::move(line).error());
  }
  if (line->empty()) {
    return std::unexpected(Status::IOError("empty Redis reply header"));
  }
  const char type = (*line)[0];
  if (type == '-') {
    return std::unexpected(Status::IOError(*line));
  }
  if (type == '+' || type == ':') {
    return std::optional<std::string>(line->substr(1));
  }
  if (type != '$') {
    return std::unexpected(Status::IOError("unexpected Redis reply type"));
  }
  if (*line == "$-1") {
    return std::optional<std::string>{};
  }
  size_t size{};
  const auto parsed = std::from_chars(line->data() + 1, line->data() + line->size(), size);
  if (parsed.ec != std::errc{} || parsed.ptr != line->data() + line->size() ||
      size > storage::kMaxValue) {
    return std::unexpected(Status::IOError("invalid/oversized Redis bulk reply"));
  }
  std::string reply(size + 2, '\0');
  if (auto status = socket.Transfer(reply.data(), reply.size(), false); !status.ok()) {
    return std::unexpected(std::move(status));
  }
  if (!reply.ends_with("\r\n")) {
    return std::unexpected(Status::IOError("invalid Redis bulk terminator"));
  }
  reply.resize(size);
  return std::optional<std::string>(std::move(reply));
}
} // namespace laminar::projection
