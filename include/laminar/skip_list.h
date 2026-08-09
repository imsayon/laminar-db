#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>

#include "laminar/arena.h"

namespace laminar {

using KeyComparator = std::function<int(const char*, const char*)>;

// A single-writer, concurrently-readable skip list. Exactly one thread may call Insert.
// Readers may run concurrently after the owner begins publishing nodes. Keys and nodes must remain
// alive until all readers finish; the owning Arena supplies that lifetime.
class SkipList {
public:
  static constexpr int kMaxHeight = 12;
  static constexpr uint32_t kBranching = 4;

  struct Node {
    Node(const char* node_key, int node_height, std::atomic<Node*>* links)
        : key(node_key), height(node_height), next(links) {}

    [[nodiscard]] Node* Next(int level) const {
      assert(level >= 0 && level < height);
      return next[level].load(std::memory_order_acquire);
    }
    void SetNext(int level, Node* node) {
      assert(level >= 0 && level < height);
      next[level].store(node, std::memory_order_release);
    }
    [[nodiscard]] Node* RelaxedNext(int level) const {
      assert(level >= 0 && level < height);
      return next[level].load(std::memory_order_relaxed);
    }
    void SetRelaxedNext(int level, Node* node) {
      assert(level >= 0 && level < height);
      next[level].store(node, std::memory_order_relaxed);
    }

    const char* key;
    int height;
    std::atomic<Node*>* next;
  };

  explicit SkipList(KeyComparator comparator, Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Returns false when an exactly equal key already exists.
  bool Insert(const char* key);
  [[nodiscard]] bool Contains(const char* key) const;

  class Iterator {
  public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    [[nodiscard]] bool Valid() const { return node_ != nullptr; }
    [[nodiscard]] const char* key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Prev();
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast();
    void Seek(const char* target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

  private:
    const SkipList* list_;
    Node* node_{nullptr};
  };

private:
  [[nodiscard]] Node* NewNode(const char* key, int height);
  [[nodiscard]] int RandomHeight();
  [[nodiscard]] bool KeyIsAfterNode(const char* key, Node* node) const;
  [[nodiscard]] Node* FindGreaterOrEqual(const char* key, Node** previous) const;
  [[nodiscard]] Node* FindLessThan(const char* key) const;
  [[nodiscard]] Node* FindLast() const;

  KeyComparator comparator_;
  Arena* arena_;
  Node* head_;
  std::atomic<int> max_height_{1};
  std::mt19937 random_{0x5eedU};
};

inline SkipList::Node* SkipList::NewNode(const char* key, int height) {
  auto* links = reinterpret_cast<std::atomic<Node*>*>(arena_->AllocateAligned(
      sizeof(std::atomic<Node*>) * static_cast<size_t>(height), alignof(std::atomic<Node*>)));
  for (int level = 0; level < height; ++level) {
    std::construct_at(&links[level], nullptr);
  }
  auto* storage = reinterpret_cast<Node*>(arena_->AllocateAligned(sizeof(Node), alignof(Node)));
  return std::construct_at(storage, key, height, links);
}

inline int SkipList::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && random_() % kBranching == 0) {
    ++height;
  }
  return height;
}

inline bool SkipList::KeyIsAfterNode(const char* key, Node* node) const {
  return node != nullptr && comparator_(node->key, key) < 0;
}

inline SkipList::Node* SkipList::FindGreaterOrEqual(const char* key, Node** previous) const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_acquire) - 1;
  while (true) {
    Node* next = node->Next(level);
    if (KeyIsAfterNode(key, next)) {
      node = next;
    } else {
      if (previous != nullptr) {
        previous[level] = node;
      }
      if (level == 0) {
        return next;
      }
      --level;
    }
  }
}

inline SkipList::Node* SkipList::FindLessThan(const char* key) const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_acquire) - 1;
  while (true) {
    Node* next = node->Next(level);
    if (next == nullptr || comparator_(next->key, key) >= 0) {
      if (level == 0) {
        return node;
      }
      --level;
    } else {
      node = next;
    }
  }
}

inline SkipList::Node* SkipList::FindLast() const {
  Node* node = head_;
  int level = max_height_.load(std::memory_order_acquire) - 1;
  while (true) {
    Node* next = node->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return node;
      }
      --level;
    } else {
      node = next;
    }
  }
}

inline SkipList::SkipList(KeyComparator comparator, Arena* arena)
    : comparator_(std::move(comparator)), arena_(arena), head_(NewNode(nullptr, kMaxHeight)) {}

inline bool SkipList::Insert(const char* key) {
  Node* previous[kMaxHeight];
  Node* node = FindGreaterOrEqual(key, previous);
  if (node != nullptr && comparator_(key, node->key) == 0) {
    return false;
  }

  const int height = RandomHeight();
  const int current_max = max_height_.load(std::memory_order_relaxed);
  if (height > current_max) {
    for (int level = current_max; level < height; ++level) {
      previous[level] = head_;
    }
    max_height_.store(height, std::memory_order_release);
  }

  node = NewNode(key, height);
  for (int level = 0; level < height; ++level) {
    node->SetRelaxedNext(level, previous[level]->RelaxedNext(level));
    previous[level]->SetNext(level, node);
  }
  return true;
}

inline bool SkipList::Contains(const char* key) const {
  Node* node = FindGreaterOrEqual(key, nullptr);
  return node != nullptr && comparator_(key, node->key) == 0;
}

inline void SkipList::Iterator::Prev() {
  assert(Valid());
  node_ = list_->FindLessThan(node_->key);
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

inline void SkipList::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

} // namespace laminar
