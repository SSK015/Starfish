#pragma once
#include "mm/cache/base/object.hpp"
#include "mm/cache/entry.hpp"
#include <cstdint>
#include <initializer_list>
#include <iostream>

namespace starfish {
namespace recompute {
template <typename T> class RemoteObject;
}
namespace cache {
class DereferenceScope {
  friend class ConcurrentArrayCache;

 protected:
  const DereferenceScope *parent;

  void unmark() const {
    pin();
    unpin();
  }
  virtual void pin() const {}
  virtual void unpin() const {}
  void enter();
  void exit();

  void recursive_pin() const {
    pin();
    if (parent) {
      parent->recursive_pin();
    }
  }

  void recursive_unpin() const {
    unpin();
    if (parent) {
      parent->recursive_unpin();
    }
  }

 public:
  explicit DereferenceScope(const DereferenceScope *parent) : parent(parent) {}

  // The definition is supplied by accessor.hpp and forwards to the default
  // cache instance.
  bool mark_recomputable(far_obj_t obj, const recompute::Inputs &inputs,
                         recompute::EntryFn callback, uint64_t arg);

  // Object-level recipe API. Protected inputs stay alive and read-only for
  // the lifetime of the output recipe; output is first in all_objects.
  template <typename T, typename Fn>
  bool mark_recomputable(
      recompute::RemoteObject<T> &output,
      std::initializer_list<recompute::RemoteObject<T>> protected_inputs,
      Fn callback,
      std::initializer_list<recompute::RemoteObject<T>> all_objects);

  // Container-level recipe API is defined by the container adapter.
  template <typename Table = void, typename Output, typename Input, typename Producer>
  bool mark_recomputable(Output &output, Input input, Producer producer);

  DereferenceScope(const DereferenceScope &) = delete;

  DereferenceScope &operator=(const DereferenceScope &) = delete;

  DereferenceScope(DereferenceScope &&) = delete;
  DereferenceScope &operator=(DereferenceScope &&) = delete;
  void recursive_unmark() const {
    unmark();
    if (parent) {
      parent->recursive_unmark();
    }
  }

  void begin_eviction() {
    recursive_pin();
    exit();
  }
  void end_eviction() {
    enter();
    recursive_unpin();
  }
};

class RootDereferenceScope : public DereferenceScope {
 public:
  RootDereferenceScope() : DereferenceScope(nullptr) { enter(); }
  ~RootDereferenceScope() { exit(); }
};

struct OnMissScope : public DereferenceScope {
  FarObjectEntry *entry;
  OnMissScope(FarObjectEntry *entry, const DereferenceScope *parent)
      : entry(entry), DereferenceScope(parent) {}
  void pin() const override { entry->pin(); }
  void unpin() const override { entry->unpin(); }
};

}  // namespace cache
};  // namespace starfish
