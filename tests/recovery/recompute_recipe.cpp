#include "mm/cache/entry.hpp"

#include <array>
#include <barrier>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

struct Context {
  explicit Context(int value) : value(value) {}
  ~Context() { ++destroyed; }

  int value;
  static int destroyed;
};

int Context::destroyed = 0;

bool add(void *destination, size_t bytes, const void *opaque, uint64_t arg) {
  if (bytes != sizeof(int) || opaque == nullptr) return false;
  *static_cast<int *>(destination) =
      static_cast<const Context *>(opaque)->value + static_cast<int>(arg);
  return true;
}

bool subtract(void *destination, size_t bytes, const void *opaque,
              uint64_t arg) {
  if (bytes != sizeof(int) || opaque == nullptr) return false;
  *static_cast<int *>(destination) =
      static_cast<const Context *>(opaque)->value - static_cast<int>(arg);
  return true;
}

}  // namespace

int main() {
  using starfish::cache::FarObjectEntry;
  using starfish::cache::LOCAL;
  using starfish::cache::MARKED;
  using starfish::cache::REMOTE;
  using starfish::recompute::Inputs;

  static_assert(sizeof(FarObjectEntry) == 48);

  Context::destroyed = 0;
  auto context = std::make_shared<Context>(40);
  Inputs inputs = Inputs::immutable_compute_owned(context);
  assert(inputs.input_state()->on_loss == nullptr);
  {
    const auto loss = +[](const void *ctx, size_t ep) {
      assert(static_cast<const Context *>(ctx)->value == 40 && ep == 2);
    };
    const auto grouped = Inputs::immutable_compute_owned(context, loss);
    for (auto fn : {add, subtract}) {
      auto *plan = grouped.input_state()->plan_for(fn);
      assert(plan && plan->on_loss == loss);
      plan->on_loss(plan->context.get(), 2);
    }
  }
  FarObjectEntry source;
  source.reset<true>(reinterpret_cast<void *>(0x1000),
                     FarObjectEntry::RemoteAddrInvalid48, sizeof(int));
  // Missing protected inputs must leave the output on its ordinary FT path.
  assert(!source.try_bind_recompute_recipe(Inputs{}, add, 2));
  assert(!source.try_bind_recompute_recipe(inputs, nullptr, 2));
  assert(!source.is_recomputable() && !source.has_recompute_recipe());
  assert(source.try_bind_recompute_recipe(inputs, add, 2));
  assert(source.has_recompute_recipe());
  assert(source.is_recomputable());

  int result = 0;
  assert(source.recompute_binding().inputs->invoke(
      &result, sizeof(result), source.recompute_binding().arg));
  assert(result == 42);

  // A bound output cannot be overwritten by a second declaration.
  assert(!source.try_bind_recompute_recipe(inputs, add, 3));
  FarObjectEntry conflict;
  conflict.reset<true>(reinterpret_cast<void *>(0x2000),
                       FarObjectEntry::RemoteAddrInvalid48, sizeof(int));
  // The same protected input can feed a different computation and argument.
  assert(conflict.try_bind_recompute_recipe(inputs, subtract, 3));
  auto second = conflict.recompute_binding();
  assert(second.inputs->invoke(&result, sizeof(result), second.arg));
  assert(result == 37);

  // A remote entry is too late to bind.
  FarObjectEntry late;
  late.reset<true>(reinterpret_cast<void *>(0x3000), 0x4000, sizeof(int));
  assert(!late.try_bind_recompute_recipe(inputs, add, 2));

  // Move transfers the intrusive reference without an extra retain/release.
  FarObjectEntry moved;
  moved.set_free();
  moved.take_six_metadata_from(source);
  moved.take_recompute_recipe_from(source);
  assert(source.recompute_binding().inputs == nullptr);
  assert(moved.has_recompute_recipe());
  context.reset();
  inputs.reset();
  assert(Context::destroyed == 0);
  result = 0;
  auto binding = moved.recompute_binding();
  assert(binding.inputs->invoke(&result, sizeof(result), binding.arg));
  assert(result == 42);

  moved.set_free();
  assert(!moved.has_recompute_recipe());
  assert(Context::destroyed == 0);
  assert(second.inputs->invoke(&result, sizeof(result), second.arg));
  assert(result == 37);
  conflict.set_free();
  assert(Context::destroyed == 1);

  // Concurrent outputs sharing a partition reuse one plan per entry point.
  {
    constexpr size_t workers = 8;
    Inputs shared = Inputs::immutable_compute_owned(std::make_shared<Context>(11));
    std::barrier start(static_cast<std::ptrdiff_t>(workers));
    std::array<std::thread, workers> threads;
    std::array<starfish::recompute::InputState *, workers> plans{};
    for (size_t i = 0; i < workers; ++i) {
      threads[i] = std::thread([&, i] {
        const bool adding = i % 2 == 0;
        start.arrive_and_wait();
        plans[i] = shared.input_state()->plan_for(adding ? add : subtract);
        int value = 0;
        assert(plans[i] && plans[i]->invoke(&value, sizeof(value), i));
        assert(value == 11 + (adding ? 1 : -1) * static_cast<int>(i));
      });
    }
    for (auto &thread : threads) thread.join();
    for (size_t i = 0; i < workers; ++i) assert(plans[i] == plans[i % 2]);
    assert(plans[0] != plans[1]);
  }

  // A flag-only annotation remains a flag-only annotation.
  FarObjectEntry marked;
  marked.reset<true>(reinterpret_cast<void *>(0x5000),
                     FarObjectEntry::RemoteAddrInvalid48, sizeof(int));
  auto state = marked.load_state();
  state.state = MARKED;
  marked.set_state(state);
  assert(marked.try_mark_recomputable_before_eviction());
  assert(marked.is_recomputable());
  assert(!marked.has_recompute_recipe());
  assert(marked.load_state().state == LOCAL);

  // No recipe can be created after a remote state transition.
  state = marked.load_state();
  state.state = REMOTE;
  marked.set_state(state);
  assert(!marked.try_bind_recompute_recipe(
      Inputs::immutable_compute_owned<Context>(std::make_shared<Context>(7)),
      add, 1));
}
