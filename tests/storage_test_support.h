#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "laminar/status.h"
#include "test_harness.h"

namespace laminar::test {

template <typename T> const Status& Error(const Result<T>& result) {
  LAMINAR_CHECK(!result);
  return result.error();
}

class Scratch {
public:
  Scratch() {
    char pattern[] = "laminar-test.XXXXXX";
    const char* created = ::mkdtemp(pattern);
    LAMINAR_CHECK(created != nullptr);
    root = std::filesystem::absolute(created);
  }

  ~Scratch() {
    if (std::uncaught_exceptions() == 0) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    } else {
      std::fprintf(stderr, "Preserved failing test directory %s\n", root.c_str());
    }
  }

  std::filesystem::path root;
};

inline void Write(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream file(path, std::ios::binary);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.close();
  LAMINAR_CHECK(file.good());
}

inline std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  LAMINAR_CHECK(file.is_open());
  return {std::istreambuf_iterator<char>(file), {}};
}

inline void InstallFixture(const std::filesystem::path& directory, std::string_view bytes) {
  std::filesystem::create_directory(directory);
  Write(directory / "LOCK", "");
  Write(directory / "data.log", bytes);
}

} // namespace laminar::test
