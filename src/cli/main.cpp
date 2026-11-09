#include <cstdio>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "laminar/db.h"
#include "storage/format.h"

namespace {

using laminar::Status;

int Fail(const Status& error) {
  std::cerr << error.ToString() << '\n';
  if (error.IsNotFound()) {
    return 2;
  }
  return error.IsInvalidArgument() ? 3 : 1;
}

int Digit(char digit) {
  if (digit >= '0' && digit <= '9') {
    return digit - '0';
  }
  if (digit >= 'a' && digit <= 'f') {
    return digit - 'a' + 10;
  }
  if (digit >= 'A' && digit <= 'F') {
    return digit - 'A' + 10;
  }
  return -1;
}

laminar::Result<std::string> DecodeHex(std::string_view hex, size_t limit) {
  if (hex.size() % 2 != 0 || hex.size() / 2 > limit) {
    return std::unexpected(Status::InvalidArgument("hex length or byte limit"));
  }
  std::string bytes;
  bytes.reserve(hex.size() / 2);
  for (size_t offset = 0; offset < hex.size(); offset += 2) {
    const int high = Digit(hex[offset]);
    const int low = Digit(hex[offset + 1]);
    if (high < 0 || low < 0) {
      return std::unexpected(Status::InvalidArgument("invalid hex digit"));
    }
    bytes.push_back(static_cast<char>((high << 4) | low));
  }
  return bytes;
}

std::string EncodeHex(std::string_view bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (const char byte : bytes) {
    const auto value = static_cast<unsigned char>(byte);
    hex.push_back(digits[value >> 4U]);
    hex.push_back(digits[value & 15U]);
  }
  return hex;
}

int Run(std::string_view command, std::string_view directory, std::string_view key_hex,
        std::string_view value_hex) {
  if (command == "inspect") {
    const auto info = laminar::Inspect(directory);
    if (!info) {
      return Fail(info.error());
    }
    std::cout << "{\"format_version\":" << info->format_version
              << ",\"log_bytes\":" << info->log_bytes << ",\"record_count\":" << info->record_count
              << ",\"live_keys\":" << info->live_keys
              << ",\"incomplete_tail_bytes\":" << info->incomplete_tail_bytes << "}\n";
  } else {
    const auto key = DecodeHex(key_hex, laminar::storage::kMaxKey);
    if (!key) {
      return Fail(key.error());
    }
    const auto value = DecodeHex(value_hex, laminar::storage::kMaxValue);
    if (!value) {
      return Fail(value.error());
    }
    auto db = laminar::DB::Open(directory);
    if (!db) {
      return Fail(db.error());
    }
    auto status = Status::OK();
    if (command == "get") {
      auto result = (*db)->Get(*key);
      if (!result) {
        status = std::move(result).error();
      } else {
        std::cout << EncodeHex(*result) << '\n';
      }
    } else {
      status = command == "put" ? (*db)->Put(*key, *value) : (*db)->Delete(*key);
    }
    const auto closed = (*db)->Close();
    if (!status.ok()) {
      return Fail(status);
    }
    if (!closed.ok()) {
      return Fail(closed);
    }
  }
  std::cout.flush();
  return std::cout ? 0 : Fail(Status::IOError("writing stdout"));
}

} // namespace

int main(int argc, char** argv) try {
  const std::string_view command = argc > 1 ? argv[1] : "";
  if ((command == "put" && argc == 5) || ((command == "get" || command == "delete") && argc == 4) ||
      (command == "inspect" && argc == 3)) {
    return Run(command, argv[2], argc >= 4 ? argv[3] : "", argc == 5 ? argv[4] : "");
  }
  std::cerr << "usage: laminar put DIRECTORY KEY_HEX VALUE_HEX\n"
               "       laminar get DIRECTORY KEY_HEX\n"
               "       laminar delete DIRECTORY KEY_HEX\n"
               "       laminar inspect DIRECTORY\n";
  return 3;
} catch (const std::bad_alloc&) {
  std::fputs("Full\n", stderr);
  return 1;
} catch (...) {
  std::fputs("IOError\n", stderr);
  return 1;
}
