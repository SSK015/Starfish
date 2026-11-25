#pragma once

// Typed remote objects for paper-style, single-output recomputation. Replay
// fetches protected inputs from their far objects under the active scope and
// publishes a private result only when the original computation succeeds.
#include "lib/debug.hpp"
#include "mm/cache/accessor.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace starfish::recompute {

template <typename T>
struct RemoteObjectState {
  static constexpr uint32_t kWriter = uint32_t{1} << 31;
  static constexpr uint32_t kReaders = kWriter - 1;

  cache::UniqueFarPtr<T> object;
  // One atomic arbitrates input-read leases against mutable accessors.
  std::atomic<uint32_t> recipe_access{0};

  bool acquire_recipe_reader() noexcept {
    auto state = recipe_access.load(std::memory_order_acquire);
    while ((state & kWriter) == 0) {
      if ((state & kReaders) == kReaders) return false;
      if (recipe_access.compare_exchange_weak(
              state, state + 1, std::memory_order_acq_rel,
              std::memory_order_acquire))
        return true;
    }
    return false;
  }
  void release_recipe_reader() noexcept {
    recipe_access.fetch_sub(1, std::memory_order_release);
  }
  bool acquire_writer() noexcept {
    uint32_t expected = 0;
    return recipe_access.compare_exchange_strong(
        expected, kWriter, std::memory_order_acq_rel,
        std::memory_order_acquire);
  }
  void release_writer() noexcept {
    recipe_access.store(0, std::memory_order_release);
  }
};

template <typename T>
struct RemoteObjectWriteLease {
  std::shared_ptr<RemoteObjectState<T>> state;
  explicit RemoteObjectWriteLease(
      std::shared_ptr<RemoteObjectState<T>> value) noexcept
      : state(std::move(value)) {}
  ~RemoteObjectWriteLease() {
    if (state) state->release_writer();
  }
};

template <typename T, bool Mut>
class ObjectAccessor {
  using Pointer = std::conditional_t<Mut, T *, const T *>;
  using Reference = std::conditional_t<Mut, T &, const T &>;
  std::shared_ptr<RemoteObjectState<T>> owner_;
  cache::LiteAccessor<T, Mut> accessor_;
  std::shared_ptr<RemoteObjectWriteLease<T>> writer_;
  bool valid_ = false;

  template <typename U> friend class RemoteObject;
  ObjectAccessor(std::shared_ptr<RemoteObjectState<T>> owner,
                 cache::LiteAccessor<T, Mut> accessor,
                 std::shared_ptr<RemoteObjectWriteLease<T>> writer = {})
      : owner_(std::move(owner)),
        accessor_(std::move(accessor)),
        writer_(std::move(writer)),
        valid_(owner_ != nullptr) {}

 public:
  ObjectAccessor() = default;
  ObjectAccessor(const ObjectAccessor &) = default;
  ObjectAccessor &operator=(const ObjectAccessor &) = default;
  ObjectAccessor(ObjectAccessor &&other) noexcept
      : owner_(std::move(other.owner_)),
        accessor_(std::move(other.accessor_)),
        writer_(std::move(other.writer_)),
        valid_(std::exchange(other.valid_, false)) {}
  ObjectAccessor &operator=(ObjectAccessor &&other) noexcept {
    if (this == &other) return *this;
    owner_ = std::move(other.owner_);
    accessor_ = std::move(other.accessor_);
    writer_ = std::move(other.writer_);
    valid_ = std::exchange(other.valid_, false);
    return *this;
  }
  bool valid() const noexcept { return valid_; }
  explicit operator bool() const noexcept { return valid(); }
  Reference operator*() const {
    ASSERT(valid_);
    return *accessor_;
  }
  Pointer operator->() const {
    ASSERT(valid_);
    return accessor_.operator->();
  }
};

template <typename T>
class RemoteObject {
  std::shared_ptr<RemoteObjectState<T>> state_ =
      std::make_shared<RemoteObjectState<T>>();

  template <typename U, typename Fn>
  friend bool cache::DereferenceScope::mark_recomputable(
      RemoteObject<U> &, std::initializer_list<RemoteObject<U>>, Fn,
      std::initializer_list<RemoteObject<U>>);

  // Keep the low-level entry identity private; callers use the typed handle.
  cache::far_obj_t obj() const noexcept {
    return state_ ? state_->object.obj() : cache::far_obj_t::null();
  }

 public:
  RemoteObject() = default;
  RemoteObject(const RemoteObject &) = default;
  RemoteObject(RemoteObject &&) noexcept = default;
  RemoteObject &operator=(const RemoteObject &) = default;
  RemoteObject &operator=(RemoteObject &&) noexcept = default;

  bool is_null() const noexcept {
    return !state_ || state_->object.is_null();
  }
  explicit operator bool() const noexcept { return !is_null(); }
  uint64_t object_id() const noexcept { return obj().obj_id; }
  size_t size() const noexcept { return state_ ? state_->object.size() : 0; }
  uint64_t remote_addr() const noexcept {
    return state_ ? state_->object.get_entry().remote_addr()
                  : cache::FarObjectEntry::RemoteAddrInvalid48;
  }
  bool is_remote() const noexcept {
    return state_ &&
           state_->object.get_entry().load_state().state == cache::REMOTE;
  }
  bool remote_invalid() const noexcept {
    return !state_ || state_->object.get_entry().remote_invalid();
  }
  bool has_recompute_recipe() const noexcept {
    return state_ && state_->object.get_entry().has_recompute_recipe();
  }

  template <typename... Args>
  ObjectAccessor<T, true> allocate_lite(cache::DereferenceScope &scope,
                                         Args &&...args) {
    if (!state_) state_ = std::make_shared<RemoteObjectState<T>>();
    if (!state_->object.is_null() || !state_->acquire_writer()) return {};
    std::shared_ptr<RemoteObjectWriteLease<T>> writer;
    try {
      writer = std::make_shared<RemoteObjectWriteLease<T>>(state_);
    } catch (...) {
      state_->release_writer();
      throw;
    }
    auto accessor =
        state_->object.allocate_lite(scope, std::forward<Args>(args)...);
    return ObjectAccessor<T, true>(state_, std::move(accessor),
                                   std::move(writer));
  }

  template <bool Mut = false>
  ObjectAccessor<T, Mut> access(cache::DereferenceScope &scope) const {
    if (!state_ || state_->object.is_null()) return {};
    if constexpr (Mut) {
      if (!state_->acquire_writer()) return {};
      std::shared_ptr<RemoteObjectWriteLease<T>> writer;
      try {
        writer = std::make_shared<RemoteObjectWriteLease<T>>(state_);
      } catch (...) {
        state_->release_writer();
        throw;
      }
      auto accessor = state_->object.template access<true>(scope);
      return ObjectAccessor<T, true>(state_, std::move(accessor),
                                     std::move(writer));
    } else {
      auto accessor = state_->object.template access<false>(scope);
      return ObjectAccessor<T, false>(state_, std::move(accessor));
    }
  }
};

template <typename Fn> struct ObjectRecipeFunction;
template <typename Ret, typename... Args>
struct ObjectRecipeFunction<Ret (*)(Args...)> {
  using Return = Ret;
  using Arguments = std::tuple<Args...>;
  static constexpr size_t arity = sizeof...(Args);
};
template <typename Ret, typename... Args>
struct ObjectRecipeFunction<Ret (*)(Args...) noexcept>
    : ObjectRecipeFunction<Ret (*)(Args...)> {};

template <typename T, typename Fn, size_t N>
struct SingleOutputRecipeContext {
  Fn callback;
  std::vector<std::shared_ptr<RemoteObjectState<T>>> inputs;

  ~SingleOutputRecipeContext() {
    for (const auto &input : inputs) input->release_recipe_reader();
  }
};

template <typename T, typename Fn, size_t N, size_t... I>
bool run_single_output_recipe(
    const SingleOutputRecipeContext<T, Fn, N> &context, void *destination,
    size_t bytes, cache::DereferenceScope &scope, std::index_sequence<I...>) {
  if (!destination || bytes != sizeof(T) || context.inputs.size() != N - 1)
    return false;

  T replayed_output{};
  // Copy each input before another fetch can advance this active scope.
  std::array<T, N - 1> input_values{};
  for (size_t i = 0; i < N - 1; ++i) {
    cache::LiteAccessor<T, false> input(
        context.inputs[i]->object, scope, cache::kNoProfile);
    input_values[i] = *input;
  }
  using Return = typename ObjectRecipeFunction<Fn>::Return;
  if constexpr (std::is_same_v<Return, bool>) {
    if (!context.callback(replayed_output, input_values[I]...)) return false;
  } else {
    static_assert(std::is_same_v<Return, void>,
                  "object callback must return void or bool");
    context.callback(replayed_output, input_values[I]...);
  }
  std::memcpy(destination, &replayed_output, sizeof(T));
  return true;
}

template <typename T, typename Fn, size_t N>
bool replay_single_output(void *destination, size_t bytes, const void *opaque,
                          uint64_t, cache::DereferenceScope *scope) {
  if (!opaque || !scope) return false;
  const auto &context =
      *static_cast<const SingleOutputRecipeContext<T, Fn, N> *>(opaque);
  return run_single_output_recipe(context, destination, bytes, *scope,
                                  std::make_index_sequence<N - 1>{});
}

}  // namespace starfish::recompute

namespace starfish::cache {

template <typename T, typename Fn>
bool DereferenceScope::mark_recomputable(
    recompute::RemoteObject<T> &output,
    std::initializer_list<recompute::RemoteObject<T>> protected_inputs,
    Fn callback,
    std::initializer_list<recompute::RemoteObject<T>> all_objects) {
  using Function = recompute::ObjectRecipeFunction<Fn>;
  constexpr size_t N = Function::arity;
  static_assert(N >= 2, "recipe callback requires output plus input");
  static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_default_constructible_v<T>,
                "recipe objects must be default-constructible and trivially copyable");
  static_assert(std::is_same_v<
                    std::tuple_element_t<0, typename Function::Arguments>, T &>,
                "callback first argument must be mutable output T&");
  static_assert(
      []<size_t... I>(std::index_sequence<I...>) {
        return (std::is_same_v<
                    std::tuple_element_t<I + 1,
                                         typename Function::Arguments>,
                    const T &> && ...);
      }(std::make_index_sequence<N - 1>{}),
      "callback inputs must be const T&");

  if (!callback || output.is_null() || !output.state_ ||
      !output.state_->acquire_writer())
    return false;
  recompute::RemoteObjectWriteLease<T> output_guard(output.state_);

  const auto &config = ::starfish::get_config();
  if (!config.exclusive_cache || !config.is_ec_batch_mode() ||
      all_objects.size() != N || protected_inputs.size() != N - 1)
    return false;

  std::vector<recompute::RemoteObject<T>> objects(all_objects);
  std::vector<recompute::RemoteObject<T>> dependencies(protected_inputs);
  if (objects.empty() || objects.front().obj() != output.obj() ||
      output.size() != sizeof(T))
    return false;

  for (size_t i = 0; i < objects.size(); ++i) {
    if (objects[i].is_null() || objects[i].size() != sizeof(T)) return false;
    for (size_t j = 0; j < i; ++j)
      if (objects[i].obj().obj_id == objects[j].obj().obj_id) return false;
  }

  auto context =
      std::make_shared<recompute::SingleOutputRecipeContext<T, Fn, N>>();
  context->callback = callback;
  context->inputs.reserve(N - 1);
  for (size_t i = 0; i < dependencies.size(); ++i) {
    const auto &dependency = dependencies[i];
    const auto &declared = objects[i + 1];
    if (dependency.is_null() || dependency.obj() != declared.obj() ||
        dependency.obj().obj_id == output.obj().obj_id || !dependency.state_ ||
        dependency.state_->object.is_null())
      return false;

    if (!dependency.state_->acquire_recipe_reader()) return false;
    context->inputs.push_back(dependency.state_);
  }

  // All mutable writers are excluded while we verify EC-backed inputs.
  for (const auto &input : context->inputs) {
    const auto &entry = input->object.get_entry();
    const auto state = entry.load_state(std::memory_order_acquire);
    if (state.state == cache::FREE || state.state == cache::BUSY ||
        entry.is_recomputable() || entry.has_recompute_recipe())
      return false;
  }

  auto immutable_context =
      std::shared_ptr<const recompute::SingleOutputRecipeContext<T, Fn, N>>(
          std::move(context));
  auto inputs =
      recompute::Inputs::immutable_compute_owned(std::move(immutable_context));
  return inputs.valid() &&
         Cache::get_default()->register_recompute_recipe(
             output.obj(), inputs,
             &recompute::replay_single_output<T, Fn, N>, 0);
}

}  // namespace starfish::cache
