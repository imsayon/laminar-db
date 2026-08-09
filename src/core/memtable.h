#pragma once

#include <memory>
#include <string>

#include "laminar/arena.h"
#include "laminar/internal_key.h"
#include "laminar/skip_list.h"
#include "laminar/slice.h"
#include "laminar/status.h"

namespace laminar {

// MemTable has one writer and may have concurrent readers. Its owner must keep it alive until all
// readers and iterators finish. Freezing and immutable-table lifecycle begin in Phase 3.
class MemTable {
public:
  static constexpr size_t kDefaultFlushThreshold = 64U * 1024U * 1024U;

  explicit MemTable(size_t flush_threshold = kDefaultFlushThreshold);
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  [[nodiscard]] Status Put(const Slice& user_key, const Slice& value, SequenceNumber sequence);
  [[nodiscard]] Status Delete(const Slice& user_key, SequenceNumber sequence);

  // Returns true when a visible value or tombstone is found. Outputs must be non-null.
  [[nodiscard]] bool Get(const Slice& user_key, SequenceNumber read_sequence, std::string* value,
                         bool* is_deleted) const;

  [[nodiscard]] bool ShouldFlush() const { return arena_.MemoryUsage() >= flush_threshold_; }
  [[nodiscard]] size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  class Iterator {
  public:
    explicit Iterator(const SkipList::Iterator& iterator) : iterator_(iterator) {}
    [[nodiscard]] bool Valid() const { return iterator_.Valid(); }
    void SeekToFirst() { iterator_.SeekToFirst(); }
    void Next() { iterator_.Next(); }
    [[nodiscard]] Slice key() const;
    [[nodiscard]] Slice value() const;

  private:
    SkipList::Iterator iterator_;
  };

  [[nodiscard]] std::unique_ptr<Iterator> NewIterator() const;

private:
  [[nodiscard]] Result<const char*> EncodeEntry(const Slice& user_key, const Slice& value,
                                                SequenceNumber sequence, ValueType type);

  Arena arena_;
  SkipList table_;
  size_t flush_threshold_;
};

} // namespace laminar
