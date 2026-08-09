#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace laminar {

class Status {
public:
  enum class Code : uint8_t {
    kOk = 0,
    kNotFound,
    kCorruption,
    kIOError,
    kInvalidArgument,
    kNotSupported,
    kAlreadyExists,
    kFull,
  };

  Status() = default;

  [[nodiscard]] static Status OK() { return {}; }
  [[nodiscard]] static Status NotFound(std::string_view message = {}) {
    return {Code::kNotFound, message};
  }
  [[nodiscard]] static Status Corruption(std::string_view message) {
    return {Code::kCorruption, message};
  }
  [[nodiscard]] static Status IOError(std::string_view message) {
    return {Code::kIOError, message};
  }
  [[nodiscard]] static Status InvalidArgument(std::string_view message) {
    return {Code::kInvalidArgument, message};
  }
  [[nodiscard]] static Status NotSupported(std::string_view message) {
    return {Code::kNotSupported, message};
  }
  [[nodiscard]] static Status AlreadyExists(std::string_view message) {
    return {Code::kAlreadyExists, message};
  }
  [[nodiscard]] static Status Full(std::string_view message) { return {Code::kFull, message}; }

  [[nodiscard]] bool ok() const { return code_ == Code::kOk; }
  [[nodiscard]] bool IsNotFound() const { return code_ == Code::kNotFound; }
  [[nodiscard]] bool IsCorruption() const { return code_ == Code::kCorruption; }
  [[nodiscard]] bool IsIOError() const { return code_ == Code::kIOError; }
  [[nodiscard]] bool IsInvalidArgument() const { return code_ == Code::kInvalidArgument; }
  [[nodiscard]] bool IsAlreadyExists() const { return code_ == Code::kAlreadyExists; }
  [[nodiscard]] bool IsFull() const { return code_ == Code::kFull; }
  [[nodiscard]] Code code() const { return code_; }
  [[nodiscard]] const std::string& message() const { return message_; }

  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const Status&, const Status&) = default;

private:
  Status(Code code, std::string_view message) : code_(code), message_(message) {}

  Code code_{Code::kOk};
  std::string message_;
};

template <typename T> using Result = std::expected<T, Status>;

#define LAMINAR_RETURN_IF_ERROR(expression)                                                        \
  do {                                                                                             \
    const ::laminar::Status laminar_status = (expression);                                         \
    if (!laminar_status.ok()) {                                                                    \
      return laminar_status;                                                                       \
    }                                                                                              \
  } while (false)

} // namespace laminar
