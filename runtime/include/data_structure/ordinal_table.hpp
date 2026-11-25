#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "async/uthreads.hpp"
#include "ft/recompute/replay_arena.hpp"

namespace starfish::recompute {

enum class OrdinalMode { plain, tracked, replay };
constexpr OrdinalMode publication_mode(bool tracked) noexcept {
  return tracked ? OrdinalMode::tracked : OrdinalMode::plain;
}

// A published ordinal identifies a key by its first insertion. Replay must
// introduce distinct keys in exactly the same order as the original computation;
// hash traversal order and rehashing are irrelevant.
template <class Key, class Value, class Hash = std::hash<Key>,
          OrdinalMode Mode = OrdinalMode::tracked>
class OrdinalTable {
 public:
  static constexpr bool tracks_ordinals = Mode != OrdinalMode::plain;
  static constexpr bool replay_build = Mode == OrdinalMode::replay;

 private:
  struct TrackedValue {
    Value value{};
    // Assigned once from the current row count on the key's first insertion.
    uint64_t ordinal{0};
  };

  // At publication the hash table carries each ordinal. At replay, the
  // position in the pointer array already is the ordinal.
  using StoredValue =
      std::conditional_t<Mode == OrdinalMode::tracked, TrackedValue, Value>;
  using Storage = std::conditional_t<replay_build,
      std::pmr::unordered_map<Key, StoredValue, Hash>,
      std::unordered_map<Key, StoredValue, Hash>>;
  using StorageEntry = typename Storage::value_type;

  struct ReplayStateEnabled {
    detail::ReplayArena arena;
    std::vector<const StorageEntry *> index;
    size_t updates_since_yield{0};
    bool sealed{false};
  };
  struct ReplayStateDisabled {};
  using ReplayState =
      std::conditional_t<replay_build, ReplayStateEnabled, ReplayStateDisabled>;

  template <class Fn>
  static bool visit_value(Fn &fn, const Key &key, const Value &value, uint64_t id) {
    if constexpr (std::is_void_v<std::invoke_result_t<Fn &, const Key &,
                                                      const Value &, uint64_t>>) {
      fn(key, value, id);
      return true;
    } else {
      return fn(key, value, id);
    }
  }

 public:
  using key_type = Key;
  using mapped_type = Value;
  using ReplayTable = OrdinalTable<Key, Value, Hash, OrdinalMode::replay>;

  // Expose the key and mapped value without copying either or exposing ordinals.
  template <bool Const>
  class RowIterator {
   private:
    using Base = std::conditional_t<Const, typename Storage::const_iterator,
                                    typename Storage::iterator>;
    Base row_{};
    explicit RowIterator(Base row) : row_(row) {}
    friend class OrdinalTable;
    template <bool> friend class RowIterator;

   public:
    using difference_type = std::ptrdiff_t;
    using value_type = std::pair<Key, Value>;
    struct reference {
      const Key &first;
      std::conditional_t<Const, const Value &, Value &> second;
      operator value_type() const { return {first, second}; }
    };
    struct pointer {
      reference row;
      const reference *operator->() const noexcept { return &row; }
    };
    using iterator_category = std::input_iterator_tag;
    using iterator_concept = std::forward_iterator_tag;

    RowIterator() = default;
    template <bool Other> requires(Const && !Other)
    RowIterator(const RowIterator<Other> &other) : row_(other.row_) {}
    reference operator*() const {
      if constexpr (Mode == OrdinalMode::tracked)
        return {row_->first, row_->second.value};
      else
        return {row_->first, row_->second};
    }
    pointer operator->() const { return {**this}; }
    RowIterator &operator++() { ++row_; return *this; }
    RowIterator operator++(int) { auto previous = *this; ++*this; return previous; }
    bool operator==(const RowIterator &) const = default;
  };

  using const_iterator = RowIterator<true>;
  using iterator = RowIterator<replay_build>;  // A sealed replay exposes immutable rows.
  iterator begin() { return iterator(rows_.begin()); }
  iterator end() { return iterator(rows_.end()); }
  const_iterator begin() const { return const_iterator(rows_.begin()); }
  const_iterator end() const { return const_iterator(rows_.end()); }
  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  OrdinalTable() requires(!replay_build) = default;
  OrdinalTable() requires replay_build : rows_(&replay_state_.arena) {}
  explicit OrdinalTable(size_t capacity) : OrdinalTable() { reserve(capacity); }
  OrdinalTable(const OrdinalTable &) = delete;
  OrdinalTable &operator=(const OrdinalTable &) = delete;
  OrdinalTable(OrdinalTable &&) = delete;
  OrdinalTable &operator=(OrdinalTable &&) = delete;

  size_t size() const noexcept { return rows_.size(); }
  bool empty() const noexcept { return rows_.empty(); }

  void reserve(size_t count) {
    require_mutable();
    rows_.reserve(count);
    if constexpr (replay_build) replay_state_.index.reserve(count);
  }

  Value &operator[](const Key &key) { return access_or_insert(key); }

  Value &operator[](Key &&key) { return access_or_insert(std::move(key)); }

  template <class Fn>
  bool visit(Fn &&fn) const {
    if constexpr (replay_build) {
      uint64_t ordinal = 0;
      for (const auto *entry : replay_state_.index)
        if (!visit_value(fn, entry->first, entry->second, ordinal++)) return false;
    } else {
      for (const auto &[key, stored] : rows_) {
        if constexpr (Mode == OrdinalMode::tracked) {
          if (!visit_value(fn, key, stored.value, stored.ordinal)) return false;
        } else {
          if (!visit_value(fn, key, stored, 0)) return false;
        }
      }
    }
    return true;
  }

  // Publishing the completed index changes the table to read-only.
  void seal_replay() requires(replay_build) {
    require_mutable();
    replay_state_.sealed = true;
  }

  // Returns false for an unindexed table or an out-of-range ordinal.
  template <class Fn>
  bool visit_ordinal(uint64_t ordinal, Fn &&fn) const
      requires(replay_build) {
    if (!replay_state_.sealed || ordinal >= replay_state_.index.size())
      return false;
    const auto &entry = *replay_state_.index[static_cast<size_t>(ordinal)];
    return visit_value(fn, entry.first, entry.second, ordinal);
  }

 private:
  static constexpr size_t kYieldInterval = 1u << 16;

  template <class K>
  Value &access_or_insert(K &&key) {
    if constexpr (replay_build) {
      require_mutable();
      cooperate_replay_build();
      auto [it, inserted] = rows_.try_emplace(std::forward<K>(key));
      if (inserted) {
        // Node addresses survive rehash. Roll back the row if extending
        // the index fails, preserving the dense first-insertion sequence.
        try {
          replay_state_.index.push_back(&*it);
        } catch (...) {
          rows_.erase(it);
          throw;
        }
      }
      return it->second;
    } else if constexpr (Mode == OrdinalMode::tracked) {
      auto [it, inserted] = rows_.try_emplace(std::forward<K>(key));
      if (inserted) it->second.ordinal = static_cast<uint64_t>(rows_.size() - 1);
      return it->second.value;
    } else {
      return rows_[std::forward<K>(key)];
    }
  }

  void require_mutable() const {
    if constexpr (replay_build) {
      if (replay_state_.sealed)
        throw std::logic_error("replay table is immutable");
    }
  }

  void cooperate_replay_build() {
    if constexpr (replay_build) {
      if (++replay_state_.updates_since_yield == kYieldInterval) {
        replay_state_.updates_since_yield = 0;
        uthread::yield();
      }
    }
  }

  // The allocator must outlive every node and bucket in rows_.
  [[no_unique_address]] ReplayState replay_state_;
  Storage rows_;
};

}  // namespace starfish::recompute
