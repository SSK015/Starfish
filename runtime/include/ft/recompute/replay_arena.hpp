#pragma once

#include <cstddef>
#include <memory>
#include <memory_resource>
#include <vector>

namespace starfish::recompute::detail {

// A prepared partition has one builder and one lifetime. Pool its small
// native allocations, and return large bucket arrays directly to the upstream
// allocator when the table rehashes.
class ReplayArena final : public std::pmr::memory_resource {
 private:
  static constexpr size_t kPageBytes = 64 * 1024;
  static constexpr size_t kMaxPooled = kPageBytes / 16;

  static bool pooled(size_t bytes, size_t alignment) noexcept {
    return bytes <= kMaxPooled && alignment <= alignof(std::max_align_t);
  }

  void *do_allocate(size_t bytes, size_t alignment) override {
    if (!pooled(bytes, alignment))
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);

    void *start = next_;
    if (!start || !std::align(alignment, bytes, start, remaining_)) {
      pages_.push_back(std::make_unique_for_overwrite<std::byte[]>(kPageBytes));
      start = pages_.back().get();
      remaining_ = kPageBytes;
    }
    next_ = static_cast<std::byte *>(start) + bytes;
    remaining_ -= bytes;
    return start;
  }

  void do_deallocate(void *pointer, size_t bytes, size_t alignment) override {
    if (!pooled(bytes, alignment))
      std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
  }

  bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }

  std::vector<std::unique_ptr<std::byte[]>> pages_;
  std::byte *next_{nullptr};
  size_t remaining_{0};
};

}  // namespace starfish::recompute::detail
