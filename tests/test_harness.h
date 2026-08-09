#pragma once

#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace laminar::test {

using TestFunction = void (*)();

struct TestCase {
  std::string_view name;
  TestFunction function;
};

class Failure final : public std::exception {
public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
  std::string message_;
};

inline void Check(bool condition, std::string_view expression, std::string_view file, int line) {
  if (!condition) {
    std::ostringstream message;
    message << file << ':' << line << ": check failed: " << expression;
    throw Failure(message.str());
  }
}

inline int Run(const std::vector<TestCase>& tests) {
  size_t passed = 0;
  for (const TestCase& test : tests) {
    std::cout << "[ RUN      ] " << test.name << '\n';
    try {
      test.function();
      ++passed;
      std::cout << "[       OK ] " << test.name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "[  FAILED  ] " << test.name << ": " << error.what() << '\n';
    } catch (...) {
      std::cerr << "[  FAILED  ] " << test.name << ": unknown exception\n";
    }
  }
  std::cout << passed << '/' << tests.size() << " tests passed\n";
  return passed == tests.size() ? 0 : 1;
}

} // namespace laminar::test

#define LAMINAR_CHECK(expression)                                                                  \
  ::laminar::test::Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define LAMINAR_CHECK_EQ(lhs, rhs) LAMINAR_CHECK((lhs) == (rhs))
#define LAMINAR_CHECK_NE(lhs, rhs) LAMINAR_CHECK((lhs) != (rhs))
