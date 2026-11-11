#pragma once

#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "laminar/redis_projection.h"
#include "projection/redis.h"
#include "storage_test_support.h"

namespace laminar::projection {
struct TestAccess {
  static Status Publish(RedisProjection& projection, ChangeCursor cursor) {
    return projection.PublishReady(cursor);
  }
};
} // namespace laminar::projection

namespace laminar::test {
class RedisServer {
public:
  RedisServer() {
    char pattern[] = "/tmp/laminar-redis.XXXXXX";
    const char* created = ::mkdtemp(pattern);
    LAMINAR_CHECK(created != nullptr);
    root = created;
    socket = (root / "redis.sock").string();
    const auto log = (root / "redis.log").string();
    const auto directory = root.string();
    pid = ::fork();
    LAMINAR_CHECK(pid >= 0);
    if (pid == 0) {
      ::execl(LAMINAR_REDIS_SERVER, LAMINAR_REDIS_SERVER, "--port", "0", "--unixsocket",
              socket.c_str(), "--unixsocketperm", "700", "--save", "", "--appendonly", "no",
              "--maxmemory-policy", "noeviction", "--dir", directory.c_str(), "--logfile",
              log.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    for (size_t i = 0; i < 100; ++i) {
      const auto ping = projection::Command(socket, std::array<std::string_view, 1>{"PING"});
      if (ping) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    Stop();
    LAMINAR_CHECK(false);
  }

  ~RedisServer() { Stop(); }
  RedisServer(const RedisServer&) = delete;
  RedisServer& operator=(const RedisServer&) = delete;

  void Stop() {
    if (pid > 0) {
      ::kill(pid, SIGCONT);
      ::kill(pid, SIGTERM);
      int status{};
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      pid = -1;
    }
    if (std::uncaught_exceptions() == 0) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    } else {
      std::fprintf(stderr, "Preserved Redis fixture %s\n", root.c_str());
    }
  }

  std::optional<std::string> Run(std::initializer_list<std::string_view> args) const {
    const auto result = projection::Command(socket, std::span(args.begin(), args.size()));
    if (!result) {
      throw Failure(result.error().ToString());
    }
    return *result;
  }
  std::filesystem::path root;
  std::string socket;
  pid_t pid{-1};
};
} // namespace laminar::test
