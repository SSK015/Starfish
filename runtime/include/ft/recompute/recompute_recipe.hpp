#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace starfish::cache {
class DereferenceScope;
}

namespace starfish::recompute {

// The callback rebuilds this output version in destination and returns false
// when it cannot produce the requested bytes.
using EntryFn = bool (*)(void *destination, size_t bytes, const void *context,
                         uint64_t arg);

// Scoped callbacks run on the requesting fibre after the output entry lock is
// released. They may read declared far inputs through this already-active scope.
using ScopedEntryFn = bool (*)(void *destination, size_t bytes,
                               const void *context, uint64_t arg,
                               cache::DereferenceScope *scope);

// Optional advisory hook, called only on a confirmed loss before output replay.
using LossFn = void (*)(const void *context, size_t failed_endpoint);

// The state shared by all Inputs wrappers for one immutable input set.  Entry
// bindings retain this object independently of the application wrapper, so a
// callback's context remains alive until every bound entry releases it.
struct InputState {
  std::shared_ptr<const void> context;
  const LossFn on_loss;
  std::atomic<size_t> refs{1};
  std::atomic<uint8_t> callback_kind{0};  // 1 = context-only, 2 = scoped
  std::atomic<EntryFn> callback{nullptr};
  std::atomic<ScopedEntryFn> scoped_callback{nullptr};

  explicit InputState(std::shared_ptr<const void> context, LossFn loss = nullptr)
      : context(std::move(context)), on_loss(loss) {}

  InputState(const InputState &) = delete;
  InputState &operator=(const InputState &) = delete;

  void retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }

  void release() noexcept {
    if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete this;
    }
  }

  bool register_callback(EntryFn fn) noexcept {
    if (!fn) return false;
    uint8_t kind = callback_kind.load(std::memory_order_acquire);
    if (kind == 0) {
      uint8_t empty = 0;
      if (callback_kind.compare_exchange_strong(
              empty, 1, std::memory_order_acq_rel,
              std::memory_order_acquire))
        kind = 1;
      else
        kind = empty;
    }
    if (kind != 1) return false;
    EntryFn entry = callback.load(std::memory_order_acquire);
    if (!entry && callback.compare_exchange_strong(
                      entry, fn, std::memory_order_acq_rel))
      return true;
    return entry == fn;
  }

  bool register_callback(ScopedEntryFn fn) noexcept {
    if (!fn) return false;
    uint8_t kind = callback_kind.load(std::memory_order_acquire);
    if (kind == 0) {
      uint8_t empty = 0;
      if (callback_kind.compare_exchange_strong(
              empty, 2, std::memory_order_acq_rel,
              std::memory_order_acquire))
        kind = 2;
      else
        kind = empty;
    }
    if (kind != 2) return false;
    ScopedEntryFn entry = scoped_callback.load(std::memory_order_acquire);
    if (!entry && scoped_callback.compare_exchange_strong(
                      entry, fn, std::memory_order_acq_rel))
      return true;
    return entry == fn;
  }

  // Bindings on one input version share a plan when they use the same
  // computation. An output with a different entry point gets its own plan.
  // Published plans stay alive as long as the input set or a binding owns them.
  InputState *plan_for(EntryFn fn) noexcept {
    return plan_for_impl(fn);
  }

  InputState *plan_for(std::nullptr_t) noexcept { return nullptr; }

  bool requires_scope() const noexcept {
    return callback_kind.load(std::memory_order_acquire) == 2;
  }

  InputState *plan_for(ScopedEntryFn fn) noexcept {
    return plan_for_impl(fn);
  }

 private:
  template <typename Fn>
  InputState *plan_for_impl(Fn fn) noexcept {
    if (!fn) return nullptr;
    for (InputState *plan = this;;) {
      if (plan->register_callback(fn)) return plan;
      InputState *next = plan->next_.load(std::memory_order_acquire);
      if (!next) {
        auto *candidate = new (std::nothrow) InputState(context, on_loss);
        if (!candidate) return nullptr;
        if (!candidate->register_callback(fn)) {
          candidate->release();
          return nullptr;
        }
        if (plan->next_.compare_exchange_strong(next, candidate,
                                                std::memory_order_acq_rel))
          return candidate;
        candidate->release();
      }
      plan = next;
    }
  }

 public:
  bool invoke(void *destination, size_t bytes, uint64_t arg) const {
    return invoke(destination, bytes, arg, nullptr);
  }

  bool invoke(void *destination, size_t bytes, uint64_t arg,
              cache::DereferenceScope *scope) const {
    const auto kind = callback_kind.load(std::memory_order_acquire);
    if (kind == 2) {
      ScopedEntryFn fn = scoped_callback.load(std::memory_order_acquire);
      return fn != nullptr && scope != nullptr &&
             fn(destination, bytes, context.get(), arg, scope);
    }
    EntryFn fn = callback.load(std::memory_order_acquire);
    return fn != nullptr && fn(destination, bytes, context.get(), arg);
  }

 private:
  std::atomic<InputState *> next_{nullptr};
  ~InputState() {
    if (auto *next = next_.load(std::memory_order_relaxed)) next->release();
  }
};

using InputSet = InputState;

class Inputs {
 private:
  struct AdoptRefTag {};

  InputState *state_ = nullptr;

  explicit Inputs(InputState *state, AdoptRefTag) noexcept : state_(state) {}

 public:
  Inputs() noexcept = default;

  Inputs(const Inputs &other) noexcept : state_(other.state_) {
    if (state_ != nullptr) state_->retain();
  }

  Inputs(Inputs &&other) noexcept
      : state_(std::exchange(other.state_, nullptr)) {}

  Inputs &operator=(const Inputs &other) noexcept {
    if (this == &other) return *this;
    Inputs copy(other);
    swap(copy);
    return *this;
  }

  Inputs &operator=(Inputs &&other) noexcept {
    if (this == &other) return *this;
    reset();
    state_ = std::exchange(other.state_, nullptr);
    return *this;
  }

  ~Inputs() { reset(); }

  template <typename T>
  static Inputs immutable_compute_owned(std::shared_ptr<const T> context,
                                        LossFn loss = nullptr) {
    if (!context) return {};
    std::shared_ptr<const void> erased(std::move(context));
    return Inputs(new InputState(std::move(erased), loss), AdoptRefTag{});
  }

  // Convenience overload for callers that still hold a mutable shared_ptr;
  // the InputState stores it as shared_ptr<const T> and exposes only const
  // context to the callback.
  template <typename T>
    requires(!std::is_const_v<T>)
  static Inputs immutable_compute_owned(std::shared_ptr<T> context,
                                        LossFn loss = nullptr) {
    return immutable_compute_owned(
        std::shared_ptr<const T>(std::move(context)), loss);
  }

  bool valid() const noexcept { return state_ != nullptr; }
  bool is_valid() const noexcept { return valid(); }
  explicit operator bool() const noexcept { return valid(); }

  InputState *state() const noexcept { return state_; }
  InputState *input_state() const noexcept { return state_; }
  InputState *owner() const noexcept { return state_; }

  void reset() noexcept {
    InputState *state = std::exchange(state_, nullptr);
    if (state != nullptr) state->release();
  }

  void swap(Inputs &other) noexcept { std::swap(state_, other.state_); }
};

// A Binding is deliberately a trivially destructible, non-owning pair.  The
// owning reference is held by the entry's InputState pointer, not by Binding.
struct Binding {
  InputState *inputs = nullptr;
  uint64_t arg = 0;
};

static_assert(sizeof(Binding) == 16);
static_assert(std::is_trivially_destructible_v<Binding>);

}  // namespace starfish::recompute
