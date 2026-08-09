#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace laminar {

// Single-owner bump allocator. Allocated memory remains valid until Arena destruction.
// Arena itself is not thread-safe; publish constructed objects through another synchronized object.
class Arena {
public:
  static constexpr size_t kBlockSize = 4096;
  static constexpr size_t kDefaultAlignment = alignof(std::max_align_t);

  Arena() = default;
  ~Arena() {
    for (const Block& block : blocks_) {
      std::free(block.data);
    }
  }

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  [[nodiscard]] char* Allocate(size_t bytes) { return AllocateAligned(bytes, kDefaultAlignment); }

  [[nodiscard]] char* AllocateAligned(size_t bytes, size_t alignment = kDefaultAlignment) {
    assert(bytes > 0);
    assert(alignment != 0 && (alignment & (alignment - 1U)) == 0);
    alignment = std::max(alignment, sizeof(void*));

    if (allocation_pointer_ != nullptr) {
      const uintptr_t address = reinterpret_cast<uintptr_t>(allocation_pointer_);
      const size_t padding = (alignment - (address & (alignment - 1U))) & (alignment - 1U);
      if (padding <= bytes_remaining_ && bytes <= bytes_remaining_ - padding) {
        char* result = allocation_pointer_ + padding;
        allocation_pointer_ += padding + bytes;
        bytes_remaining_ -= padding + bytes;
        return result;
      }
    }
    return AllocateFallback(bytes, alignment);
  }

  [[nodiscard]] size_t MemoryUsage() const { return memory_usage_.load(std::memory_order_relaxed); }

private:
  struct Block {
    void* data;
    size_t size;
  };

  [[nodiscard]] char* AllocateFallback(size_t bytes, size_t alignment) {
    if (bytes > kBlockSize / 4 || alignment > kDefaultAlignment) {
      return AllocateBlock(bytes, alignment, false);
    }
    return AllocateBlock(kBlockSize, kDefaultAlignment, true, bytes);
  }

  [[nodiscard]] char* AllocateBlock(size_t block_size, size_t alignment, bool make_current,
                                    size_t consumed = 0) {
    void* memory = nullptr;
    if (::posix_memalign(&memory, alignment, block_size) != 0 || memory == nullptr) {
      throw std::bad_alloc();
    }
    blocks_.push_back({memory, block_size});
    memory_usage_.fetch_add(block_size, std::memory_order_relaxed);
    auto* result = static_cast<char*>(memory);
    if (make_current) {
      allocation_pointer_ = result + consumed;
      bytes_remaining_ = block_size - consumed;
    }
    return result;
  }

  char* allocation_pointer_{nullptr};
  size_t bytes_remaining_{0};
  std::atomic<size_t> memory_usage_{0};
  std::vector<Block> blocks_;
};

} // namespace laminar
