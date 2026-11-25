#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace starfish::recompute {

// Recovery-only wall-clock metrics. The runtime reads STARFISH_RECIPE_METRICS
// once and prints one summary when the runtime is destroyed.
class RecoveryStats {
 public:
  explicit RecoveryStats(bool enabled = false) noexcept : enabled_(enabled) {}

  static bool metrics_requested() noexcept {
    const char *value = std::getenv("STARFISH_RECIPE_METRICS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }

  static RecoveryStats &global() noexcept {
    static RecoveryStats stats(metrics_requested());
    return stats;
  }

  bool enabled() const noexcept { return enabled_; }

 private:
  static uint64_t now_ns() noexcept {
    const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }

  class UnionMeter {
   public:
    uint64_t enter() noexcept {
      for (;;) {
        uint64_t state = active_.load(std::memory_order_acquire);
        if ((state & kTransition) != 0) {
          std::atomic_signal_fence(std::memory_order_seq_cst);
          continue;
        }

        const uint64_t count = state & kCountMask;
        if (count == 0) {
          uint64_t expected = 0;
          if (!active_.compare_exchange_weak(
                  expected, kTransition, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            continue;
          }
          start_ns_ = now_ns();
          active_.store(1, std::memory_order_release);
          update_peak(1);
          return 1;
        }

        assert(count < kCountMask);
        uint64_t expected = state;
        if (active_.compare_exchange_weak(
                expected, state + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          update_peak(count + 1);
          return count + 1;
        }
      }
    }

    void leave() noexcept {
      for (;;) {
        uint64_t state = active_.load(std::memory_order_acquire);
        if ((state & kTransition) != 0) {
          std::atomic_signal_fence(std::memory_order_seq_cst);
          continue;
        }

        const uint64_t count = state & kCountMask;
        assert(count != 0);
        if (count > 1) {
          uint64_t expected = state;
          if (active_.compare_exchange_weak(
                  expected, state - 1, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            return;
          }
          continue;
        }

        uint64_t expected = state;
        if (!active_.compare_exchange_weak(
                expected, kTransition, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          continue;
        }

        const uint64_t end_ns = now_ns();
        assert(end_ns >= start_ns_);
        union_ns_.fetch_add(end_ns - start_ns_, std::memory_order_relaxed);
        active_.store(0, std::memory_order_release);
        return;
      }
    }

    uint64_t union_ns() const noexcept {
      return union_ns_.load(std::memory_order_relaxed);
    }

    uint64_t peak() const noexcept {
      return peak_.load(std::memory_order_relaxed);
    }

   private:
    static constexpr uint64_t kTransition = uint64_t{1} << 63;
    static constexpr uint64_t kCountMask = ~kTransition;

    void update_peak(uint64_t value) noexcept {
      uint64_t observed = peak_.load(std::memory_order_relaxed);
      while (observed < value &&
             !peak_.compare_exchange_weak(observed, value,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
      }
    }

    std::atomic<uint64_t> active_{0};
    uint64_t start_ns_{0};
    std::atomic<uint64_t> union_ns_{0};
    std::atomic<uint64_t> peak_{0};
  };

  class TimedScope {
   public:
    TimedScope(RecoveryStats &owner, UnionMeter &kind_union,
               std::atomic<uint64_t> &work_sum,
               std::atomic<uint64_t> &scope_count) noexcept
        : owner_(owner), kind_union_(kind_union), work_sum_(work_sum),
          scope_count_(scope_count), active_(owner.enabled_) {
      if (!active_) return;
      owner_.combined_union_.enter();
      kind_union_.enter();
      start_ns_ = now_ns();
    }

    ~TimedScope() {
      if (!active_) return;
      const uint64_t end_ns = now_ns();
      assert(end_ns >= start_ns_);
      work_sum_.fetch_add(end_ns - start_ns_, std::memory_order_relaxed);
      scope_count_.fetch_add(1, std::memory_order_relaxed);
      kind_union_.leave();
      owner_.combined_union_.leave();
    }

    TimedScope(const TimedScope &) = delete;
    TimedScope &operator=(const TimedScope &) = delete;

   private:
    RecoveryStats &owner_;
    UnionMeter &kind_union_;
    std::atomic<uint64_t> &work_sum_;
    std::atomic<uint64_t> &scope_count_;
    uint64_t start_ns_{0};
    bool active_{false};
  };

 public:
  struct Snapshot {
    uint64_t preparation_batches{0};
    uint64_t partition_build_attempts{0};
    uint64_t rebuilt_partitions{0};
    uint64_t failed_partition_builds{0};
    uint64_t callbacks{0};
    uint64_t peak_builders{0};
    uint64_t peak_callbacks{0};
    uint64_t batch_sum_ns{0};
    uint64_t batch_wall_ns{0};
    uint64_t build_sum_ns{0};
    uint64_t build_wall_ns{0};
    uint64_t callback_sum_ns{0};
    uint64_t callback_wall_ns{0};
    uint64_t recovery_wall_ns{0};
  };

  class Batch {
   public:
    explicit Batch(RecoveryStats &stats) noexcept
        : scope_(stats, stats.batch_union_, stats.batch_sum_ns_,
                 stats.preparation_batches_) {}
    Batch(const Batch &) = delete;
    Batch &operator=(const Batch &) = delete;

   private:
    TimedScope scope_;
  };

  class Build {
   public:
    explicit Build(RecoveryStats &stats) noexcept
        : stats_(stats),
          scope_(stats, stats.build_union_, stats.build_sum_ns_,
                 stats.partition_build_attempts_) {}
    ~Build() { failure(); }

    Build(const Build &) = delete;
    Build &operator=(const Build &) = delete;

    void success() noexcept {
      if (completion_recorded_) return;
      completion_recorded_ = true;
      if (stats_.enabled_) {
        stats_.rebuilt_partitions_.fetch_add(1, std::memory_order_relaxed);
      }
    }

    void failure() noexcept {
      if (completion_recorded_) return;
      completion_recorded_ = true;
      if (stats_.enabled_) {
        stats_.failed_partition_builds_.fetch_add(1,
                                                   std::memory_order_relaxed);
      }
    }

   private:
    RecoveryStats &stats_;
    TimedScope scope_;
    bool completion_recorded_{false};
  };

  class Callback {
   public:
    explicit Callback(RecoveryStats &stats) noexcept
        : scope_(stats, stats.callback_union_, stats.callback_sum_ns_,
                 stats.callbacks_) {}
    Callback(const Callback &) = delete;
    Callback &operator=(const Callback &) = delete;

   private:
    TimedScope scope_;
  };

  Snapshot snapshot() const noexcept {
    Snapshot result;
    result.preparation_batches =
        preparation_batches_.load(std::memory_order_relaxed);
    result.partition_build_attempts =
        partition_build_attempts_.load(std::memory_order_relaxed);
    result.rebuilt_partitions =
        rebuilt_partitions_.load(std::memory_order_relaxed);
    result.failed_partition_builds =
        failed_partition_builds_.load(std::memory_order_relaxed);
    result.callbacks = callbacks_.load(std::memory_order_relaxed);
    result.peak_builders = build_union_.peak();
    result.peak_callbacks = callback_union_.peak();
    result.batch_sum_ns = batch_sum_ns_.load(std::memory_order_relaxed);
    result.batch_wall_ns = batch_union_.union_ns();
    result.build_sum_ns = build_sum_ns_.load(std::memory_order_relaxed);
    result.build_wall_ns = build_union_.union_ns();
    result.callback_sum_ns = callback_sum_ns_.load(std::memory_order_relaxed);
    result.callback_wall_ns = callback_union_.union_ns();
    result.recovery_wall_ns = combined_union_.union_ns();
    return result;
  }

  void print(uint64_t registered, uint64_t restored_objects,
             uint64_t failures) const {
    if (!enabled_) return;
    const auto value = snapshot();
    std::cout << "starfish_recipe_recovery"
              << " registered=" << registered
              << " restored_objects=" << restored_objects
              << " failures=" << failures
              << " rebuilt_partitions=" << value.rebuilt_partitions
              << " failed_partition_builds=" << value.failed_partition_builds
              << " callbacks=" << value.callbacks
              << " preparation_batches=" << value.preparation_batches
              << " peak_builders=" << value.peak_builders
              << " peak_callbacks=" << value.peak_callbacks
              << " recovery_wall_ns=" << value.recovery_wall_ns
              << " batch_wall_ns=" << value.batch_wall_ns
              << " build_wall_ns=" << value.build_wall_ns
              << " callback_wall_ns=" << value.callback_wall_ns
              << " build_sum_ns=" << value.build_sum_ns
              << " callback_sum_ns=" << value.callback_sum_ns << '\n';
  }

 private:
  const bool enabled_;
  UnionMeter batch_union_;
  UnionMeter build_union_;
  UnionMeter callback_union_;
  UnionMeter combined_union_;
  std::atomic<uint64_t> preparation_batches_{0};
  std::atomic<uint64_t> partition_build_attempts_{0};
  std::atomic<uint64_t> rebuilt_partitions_{0};
  std::atomic<uint64_t> failed_partition_builds_{0};
  std::atomic<uint64_t> callbacks_{0};
  std::atomic<uint64_t> batch_sum_ns_{0};
  std::atomic<uint64_t> build_sum_ns_{0};
  std::atomic<uint64_t> callback_sum_ns_{0};
};

}  // namespace starfish::recompute