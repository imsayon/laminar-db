#include "core/memtable.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>

#include "laminar/coding.h"

namespace laminar {
namespace {

Slice GetTrustedLengthPrefixedSlice(const char* data) {
  uint32_t length = 0;
  const char* value = GetVarint32Ptr(data, data + 5, &length);
  assert(value != nullptr);
  return {value, length};
}

} // namespace

MemTable::MemTable(size_t flush_threshold)
    : table_(
          [](const char* lhs, const char* rhs) {
            return InternalKeyCompare(GetTrustedLengthPrefixedSlice(lhs),
                                      GetTrustedLengthPrefixedSlice(rhs));
          },
          &arena_),
      flush_threshold_(flush_threshold) {}

Result<const char*> MemTable::EncodeEntry(const Slice& user_key, const Slice& value,
                                          SequenceNumber sequence, ValueType type) {
  if (user_key.size() > std::numeric_limits<uint32_t>::max() - 8U) {
    return std::unexpected(Status::InvalidArgument("MemTable key is too large"));
  }
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    return std::unexpected(Status::InvalidArgument("MemTable value is too large"));
  }

  std::string internal_key;
  const Status key_status = AppendInternalKey(&internal_key, user_key, sequence, type);
  if (!key_status.ok()) {
    return std::unexpected(key_status);
  }

  const size_t allocation_size = 5U + internal_key.size() + 5U + value.size();
  try {
    char* buffer = arena_.Allocate(allocation_size);
    char* cursor = EncodeVarint32(buffer, static_cast<uint32_t>(internal_key.size()));
    std::memcpy(cursor, internal_key.data(), internal_key.size());
    cursor += internal_key.size();
    cursor = EncodeVarint32(cursor, static_cast<uint32_t>(value.size()));
    if (!value.empty()) {
      std::memcpy(cursor, value.data(), value.size());
    }
    return buffer;
  } catch (const std::bad_alloc&) {
    return std::unexpected(Status::Full("MemTable arena allocation failed"));
  }
}

Status MemTable::Put(const Slice& user_key, const Slice& value, SequenceNumber sequence) {
  auto entry = EncodeEntry(user_key, value, sequence, kTypeValue);
  if (!entry) {
    return entry.error();
  }
  if (!table_.Insert(*entry)) {
    return Status::AlreadyExists("duplicate MemTable value entry");
  }
  return Status::OK();
}

Status MemTable::Delete(const Slice& user_key, SequenceNumber sequence) {
  auto entry = EncodeEntry(user_key, Slice{}, sequence, kTypeDeletion);
  if (!entry) {
    return entry.error();
  }
  if (!table_.Insert(*entry)) {
    return Status::AlreadyExists("duplicate MemTable deletion entry");
  }
  return Status::OK();
}

bool MemTable::Get(const Slice& user_key, SequenceNumber read_sequence, std::string* value,
                   bool* is_deleted) const {
  assert(value != nullptr);
  assert(is_deleted != nullptr);
  *is_deleted = false;

  if (read_sequence > kMaxSequenceNumber) {
    return false;
  }

  std::string internal_key;
  if (!AppendInternalKey(&internal_key, user_key, read_sequence, kTypeValue).ok()) {
    return false;
  }
  std::string lookup_entry;
  PutVarint32(&lookup_entry, static_cast<uint32_t>(internal_key.size()));
  lookup_entry.append(internal_key);

  SkipList::Iterator iterator(&table_);
  iterator.Seek(lookup_entry.data());
  if (!iterator.Valid()) {
    return false;
  }

  const Slice entry_key = GetTrustedLengthPrefixedSlice(iterator.key());
  ParsedInternalKey parsed;
  if (!ParseInternalKey(entry_key, &parsed) || parsed.user_key != user_key) {
    return false;
  }
  if (parsed.type == kTypeDeletion) {
    *is_deleted = true;
    return true;
  }

  const char* cursor = iterator.key();
  uint32_t key_length = 0;
  cursor = GetVarint32Ptr(cursor, cursor + 5, &key_length);
  assert(cursor != nullptr);
  cursor += key_length;
  *value = GetTrustedLengthPrefixedSlice(cursor).ToString();
  return true;
}

Slice MemTable::Iterator::key() const { return GetTrustedLengthPrefixedSlice(iterator_.key()); }

Slice MemTable::Iterator::value() const {
  const char* cursor = iterator_.key();
  uint32_t key_length = 0;
  cursor = GetVarint32Ptr(cursor, cursor + 5, &key_length);
  assert(cursor != nullptr);
  cursor += key_length;
  return GetTrustedLengthPrefixedSlice(cursor);
}

std::unique_ptr<MemTable::Iterator> MemTable::NewIterator() const {
  return std::make_unique<Iterator>(SkipList::Iterator(&table_));
}

} // namespace laminar
