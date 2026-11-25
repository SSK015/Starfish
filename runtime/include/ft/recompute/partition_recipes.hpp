#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "async/fork_join.hpp"
#include "ft/recompute/partition_recovery.hpp"
#include "ft/recompute/recompute_recipe.hpp"
#include "ft/recompute/recovery_stats.hpp"

namespace starfish::recompute {

namespace detail {

class PartitionRecipeRegistry;

class PartitionRecipeGroup {
 public:
  virtual ~PartitionRecipeGroup() = default;
  virtual bool is_lost(size_t endpoint) const = 0;
  virtual void prepare() const = 0;
  virtual bool replay(void *destination, size_t bytes, uint64_t locator) const = 0;
  virtual std::shared_ptr<PartitionRecipeRegistry> registry() const = 0;
};

class PartitionRecipeRegistry {
 private:
  struct Slot {
    std::mutex mutex;
    std::weak_ptr<const PartitionRecipeGroup> group;
  };

 public:
  PartitionRecipeRegistry(size_t endpoints, size_t partitions, size_t workers)
      : recovery_(endpoints, partitions, workers),
        partitions_(partitions),
        slots_(std::make_unique<Slot[]>(partitions)) {}

  void register_group(
      size_t partition,
      const std::shared_ptr<const PartitionRecipeGroup> &group) {
    if (partition >= partitions_) {
      throw std::out_of_range("recipe partition index is out of range");
    }

    auto &slot = slots_[partition];
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (auto current = slot.group.lock(); current && current != group) {
      throw std::logic_error("recipe partition is already registered");
    }
    slot.group = group;
  }

  void prepare_lost_partitions(
      size_t endpoint, const PartitionRecipeGroup *trigger) {
    recovery_.prepare_once(
        endpoint,
        [&](size_t partition) {
          std::shared_ptr<const PartitionRecipeGroup> group;
          {
            auto &slot = slots_[partition];
            std::lock_guard<std::mutex> lock(slot.mutex);
            group = slot.group.lock();
          }
          if (group != nullptr &&
              (group.get() == trigger || group->is_lost(endpoint))) {
            group->prepare();
          }
        },
        [&](auto &&dispatch) {
          RecoveryStats::Batch batch(RecoveryStats::global());
          dispatch();
        });
  }

 private:
  PartitionRecovery recovery_;
  size_t partitions_;
  std::unique_ptr<Slot[]> slots_;
};

template <typename Input, typename Build, typename Lost, typename Project>
class TypedPartitionRecipeGroup final : public PartitionRecipeGroup {
 public:
  using Prepared =
      typename std::remove_reference_t<decltype(
          std::declval<Build &>()(std::declval<const Input &>()))>::element_type;

  static_assert(std::is_same_v<
                std::remove_cvref_t<decltype(
                    std::declval<Build &>()(
                        std::declval<const Input &>()))>,
                std::unique_ptr<Prepared>>,
                "recipe builder must return std::unique_ptr<Prepared>");

  TypedPartitionRecipeGroup(
      std::shared_ptr<PartitionRecipeRegistry> registry, Input input,
      Build build, Lost lost, Project project)
      : registry_(std::move(registry)),
        input_(std::move(input)),
        build_(std::move(build)),
        lost_(std::move(lost)),
        project_(std::move(project)) {}

  void register_partition(
      size_t partition,
      const std::shared_ptr<const TypedPartitionRecipeGroup> &self) {
    registry_->register_group(
        partition, std::static_pointer_cast<const PartitionRecipeGroup>(self));
  }

  bool is_lost(size_t endpoint) const override { return lost_(endpoint); }

  void prepare() const override { (void)ensure_prepared(true); }

  std::shared_ptr<PartitionRecipeRegistry> registry() const override {
    return registry_;
  }

  bool replay(void *destination, size_t bytes, uint64_t locator) const {
    RecoveryStats::Callback callback(RecoveryStats::global());
    const Prepared *table = ensure_prepared(false);
    if (table == nullptr) return false;
    return project_(input_, *table, locator, destination, bytes);
  }

 private:
  const Prepared *ensure_prepared(bool speculative) const {
    bool waited_for_builder = false;
    for (;;) {
      const uint8_t current = state_.load(std::memory_order_acquire);
      if (current == kReady) return prepared_.get();
      if (current == kFailed) return nullptr;
      if (current == kBuilding) {
        waited_for_builder = true;
        uthread::yield();
        continue;
      }
      if (speculative && waited_for_builder) return nullptr;

      uint8_t expected = kEmpty;
      if (!state_.compare_exchange_strong(
              expected, kBuilding, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        continue;
      }

      RecoveryStats::Build build_stats(RecoveryStats::global());
      try {
        auto prepared = build_(input_);
        if (prepared == nullptr) return fail(speculative);
        prepared_ = std::move(prepared);
        build_stats.success();
        state_.store(kReady, std::memory_order_release);
        return prepared_.get();
      } catch (...) {
        return fail(speculative);
      }
    }
  }

  const Prepared *fail(bool speculative) const {
    state_.store(speculative ? kEmpty : kFailed, std::memory_order_release);
    return nullptr;
  }

  static constexpr uint8_t kEmpty = 0;
  static constexpr uint8_t kBuilding = 1;
  static constexpr uint8_t kReady = 2;
  static constexpr uint8_t kFailed = 3;

  std::shared_ptr<PartitionRecipeRegistry> registry_;
  const Input input_;
  const Build build_;
  const Lost lost_;
  const Project project_;
  mutable std::atomic<uint8_t> state_{kEmpty};
  mutable std::unique_ptr<Prepared> prepared_;
};

}  // namespace detail

// Owns per-partition recovery plans, shared prepared tables, and endpoint-loss
// preparation. Application code supplies the immutable input, deterministic
// builder, loss predicate, and output projection for each partition.
class PartitionRecipes {
 private:
  // Copies publish into the same registry, allocated on first binding.
  struct LazyRegistry {
    LazyRegistry(size_t endpoints, size_t partitions, size_t workers)
        : endpoint_count(endpoints), partition_count(partitions),
          worker_count(workers) {}

    const size_t endpoint_count, partition_count, worker_count;
    std::once_flag once;
    std::shared_ptr<detail::PartitionRecipeRegistry> registry;
  };

 public:
  PartitionRecipes(size_t endpoint_count, size_t partitions, size_t workers)
      : lazy_(std::make_shared<LazyRegistry>(endpoint_count, partitions,
                                             workers)) {}

  template <typename Input, typename Build, typename Lost, typename Project>
  Inputs bind(size_t partition, Input input, Build build, Lost lost,
              Project project) const {
    using Group = detail::TypedPartitionRecipeGroup<
        Input, std::decay_t<Build>, std::decay_t<Lost>, std::decay_t<Project>>;
    auto group = std::make_shared<Group>(registry(), std::move(input),
                                         std::move(build), std::move(lost),
                                         std::move(project));
    group->register_partition(partition, group);
    std::shared_ptr<const detail::PartitionRecipeGroup> context = group;
    return Inputs::immutable_compute_owned(
        std::move(context), &PartitionRecipes::on_loss);
  }

  static bool replay(void *destination, size_t bytes, const void *context,
                     uint64_t locator) {
    if (context == nullptr || destination == nullptr) return false;
    return static_cast<const detail::PartitionRecipeGroup *>(context)->replay(
        destination, bytes, locator);
  }

  static void on_loss(const void *context, size_t endpoint) {
    if (context == nullptr) return;
    auto registry =
        static_cast<const detail::PartitionRecipeGroup *>(context)->registry();
    if (registry != nullptr) {
      registry->prepare_lost_partitions(
          endpoint,
          static_cast<const detail::PartitionRecipeGroup *>(context));
    }
  }

 private:
  std::shared_ptr<detail::PartitionRecipeRegistry> registry() const {
    // Different partitions may publish concurrently. A failed allocation
    // leaves initialization retryable; no partially built registry escapes.
    auto &state = *lazy_;
    std::call_once(state.once, [&state] {
      state.registry = std::make_shared<detail::PartitionRecipeRegistry>(
          state.endpoint_count, state.partition_count, state.worker_count);
    });
    return state.registry;
  }

  std::shared_ptr<LazyRegistry> lazy_;
};

}  // namespace starfish::recompute
