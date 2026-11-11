#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
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

Status AddBatchLine(laminar::WriteBatch& batch, std::string_view line) {
  const size_t command_end = line.find('\t');
  if (command_end == std::string_view::npos) {
    return Status::InvalidArgument("invalid batch line");
  }
  const auto command = line.substr(0, command_end);
  const auto fields = line.substr(command_end + 1);
  const size_t key_end = fields.find('\t');
  const bool has_value = key_end != std::string_view::npos;
  if ((has_value && command != "put") || (!has_value && command != "delete")) {
    return Status::InvalidArgument("invalid batch operation or field count");
  }
  auto key = DecodeHex(fields.substr(0, key_end), laminar::storage::kMaxKey);
  if (!key) {
    return std::move(key).error();
  }
  if (!has_value) {
    return batch.Delete(*key);
  }
  auto value = DecodeHex(fields.substr(key_end + 1), laminar::storage::kMaxValue);
  return value ? batch.Put(*key, *value) : std::move(value).error();
}

Status ReadBatch(const char* path, laminar::WriteBatch& batch) {
  constexpr size_t kMaxInput = 2 * laminar::storage::kMaxPayload + 65'536;
  const std::unique_ptr<std::FILE, int (*)(std::FILE*)> file(std::fopen(path, "rb"), std::fclose);
  if (!file) {
    return Status::IOError("opening batch input");
  }
  std::string input;
  std::array<char, 8192> buffer{};
  do {
    const size_t remaining = kMaxInput - input.size();
    const size_t count =
        std::fread(buffer.data(), 1, std::min(buffer.size(), remaining + 1), file.get());
    if (std::ferror(file.get()) != 0) {
      return Status::IOError("reading batch input");
    }
    if (count > remaining) {
      return Status::InvalidArgument("batch input size limit");
    }
    input.append(buffer.data(), count);
  } while (std::feof(file.get()) == 0);

  std::string_view rest(input);
  while (!rest.empty()) {
    const size_t end = rest.find('\n');
    auto line = rest.substr(0, end);
    if (end != std::string_view::npos && line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    LAMINAR_RETURN_IF_ERROR(AddBatchLine(batch, line));
    if (end == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(end + 1);
  }
  return Status::OK();
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

int Run(std::string_view command, std::string_view directory, const char* argument,
        std::string_view value_hex) {
  if (command == "compact") {
    const auto status = laminar::Compact(directory);
    if (!status.ok()) {
      return Fail(status);
    }
  } else if (command == "inspect") {
    const auto info = laminar::Inspect(directory);
    if (!info) {
      return Fail(info.error());
    }
    std::cout << "{\"format_version\":" << info->format_version
              << ",\"log_bytes\":" << info->log_bytes << ",\"record_count\":" << info->record_count
              << ",\"live_keys\":" << info->live_keys
              << ",\"incomplete_tail_bytes\":" << info->incomplete_tail_bytes << "}\n";
  } else {
    laminar::WriteBatch batch;
    std::string key;
    std::string value;
    if (command == "batch") {
      if (const auto status = ReadBatch(argument, batch); !status.ok()) {
        return Fail(status);
      }
    } else {
      auto decoded_key = DecodeHex(argument, laminar::storage::kMaxKey);
      if (!decoded_key) {
        return Fail(decoded_key.error());
      }
      auto decoded_value = DecodeHex(value_hex, laminar::storage::kMaxValue);
      if (!decoded_value) {
        return Fail(decoded_value.error());
      }
      key = std::move(*decoded_key);
      value = std::move(*decoded_value);
    }
    auto db = laminar::DB::Open(directory);
    if (!db) {
      return Fail(db.error());
    }
    auto status = Status::OK();
    if (command == "get") {
      auto result = (*db)->Get(key);
      if (!result) {
        status = std::move(result).error();
      } else {
        std::cout << EncodeHex(*result) << '\n';
      }
    } else if (command == "batch") {
      status = (*db)->Write(batch);
    } else {
      status = command == "put" ? (*db)->Put(key, value) : (*db)->Delete(key);
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
  if ((command == "put" && argc == 5) ||
      ((command == "get" || command == "delete" || command == "batch") && argc == 4) ||
      ((command == "inspect" || command == "compact") && argc == 3)) {
    return Run(command, argv[2], argc >= 4 ? argv[3] : "", argc == 5 ? argv[4] : "");
  }
  std::cerr << "usage: laminar put DIRECTORY KEY_HEX VALUE_HEX\n"
               "       laminar get DIRECTORY KEY_HEX\n"
               "       laminar delete DIRECTORY KEY_HEX\n"
               "       laminar batch DIRECTORY INPUT_FILE\n"
               "       laminar inspect DIRECTORY\n"
               "       laminar compact DIRECTORY\n";
  return 3;
} catch (const std::bad_alloc&) {
  std::fputs("Full\n", stderr);
  return 1;
} catch (...) {
  std::fputs("IOError\n", stderr);
  return 1;
}
