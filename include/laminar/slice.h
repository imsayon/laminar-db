#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace laminar {

class Slice {
public:
  constexpr Slice() = default;
  constexpr Slice(const char* data, size_t size) : data_(data), size_(size) {
    assert(data != nullptr || size == 0);
  }
  Slice(const std::string& value) : data_(value.data()), size_(value.size()) {}         // NOLINT
  constexpr Slice(std::string_view value) : data_(value.data()), size_(value.size()) {} // NOLINT
  Slice(const char* value)
      : data_(value), size_(value == nullptr ? 0 : std::strlen(value)) {} // NOLINT

  [[nodiscard]] constexpr const char* data() const { return data_; }
  [[nodiscard]] constexpr size_t size() const { return size_; }
  [[nodiscard]] constexpr bool empty() const { return size_ == 0; }

  [[nodiscard]] char operator[](size_t index) const {
    assert(index < size_);
    return data_[index];
  }

  void remove_prefix(size_t count) {
    assert(count <= size_);
    if (count != 0) {
      data_ += count;
    }
    size_ -= count;
  }

  [[nodiscard]] std::string ToString() const {
    return size_ == 0 ? std::string{} : std::string(data_, size_);
  }
  [[nodiscard]] std::string_view ToStringView() const {
    return size_ == 0 ? std::string_view{} : std::string_view(data_, size_);
  }

  [[nodiscard]] int Compare(const Slice& other) const {
    const size_t common = std::min(size_, other.size_);
    const int result = common == 0 ? 0 : std::memcmp(data_, other.data_, common);
    if (result != 0) {
      return result;
    }
    if (size_ < other.size_) {
      return -1;
    }
    return size_ > other.size_ ? 1 : 0;
  }

  friend bool operator==(const Slice& lhs, const Slice& rhs) {
    return lhs.size_ == rhs.size_ &&
           (lhs.size_ == 0 || std::memcmp(lhs.data_, rhs.data_, lhs.size_) == 0);
  }
  friend bool operator!=(const Slice& lhs, const Slice& rhs) { return !(lhs == rhs); }
  friend bool operator<(const Slice& lhs, const Slice& rhs) { return lhs.Compare(rhs) < 0; }
  friend bool operator<=(const Slice& lhs, const Slice& rhs) { return lhs.Compare(rhs) <= 0; }
  friend bool operator>(const Slice& lhs, const Slice& rhs) { return lhs.Compare(rhs) > 0; }
  friend bool operator>=(const Slice& lhs, const Slice& rhs) { return lhs.Compare(rhs) >= 0; }

private:
  const char* data_{nullptr};
  size_t size_{0};
};

} // namespace laminar
