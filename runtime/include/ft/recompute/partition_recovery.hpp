#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "async/fork_join.hpp"

namespace starfish::recompute {

// Coordinate application-declared, independent immutable inputs on first loss.
// Helpers must only inspect local metadata and prepare native state. They must
// not dereference far objects or wait for the calling fibre to release a pin.
class PartitionRecovery {
 public:
  PartitionRecovery(size_t endpoints, size_t partitions, size_t workers)
      : endpoints_(endpoints), partitions_(partitions), workers_(workers),
        started_(std::make_unique<std::atomic<bool>[]>(endpoints)) {
    for (size_t i = 0; i < endpoints; ++i)
      started_[i].store(false, std::memory_order_relaxed);
  }

  template <typename Prepare>
  bool prepare_once(size_t endpoint, Prepare &&prepare) {
    return prepare_once(endpoint, std::forward<Prepare>(prepare),
                        [](auto &&dispatch) { dispatch(); });
  }

  // The wrapper runs only for the thread that wins the endpoint election.
  template <typename Prepare, typename RunBatch>
  bool prepare_once(size_t endpoint, Prepare &&prepare, RunBatch &&run_batch) {
    if (endpoint >= endpoints_ || !partitions_ ||
        started_[endpoint].load(std::memory_order_acquire)) return false;
    bool expected = false;
    if (!started_[endpoint].compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) return false;

    // A concurrent demand never waits on this batch. It may still build its
    // own input via the authoritative per-partition initializer.
    run_batch([&] {
      const size_t workers = std::max<size_t>(
          1, std::min({workers_, partitions_, uthread::get_worker_count()}));
      std::atomic<size_t> next{0};
      auto work = [&](size_t) {
        for (;;) {
          const size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= partitions_) break;
          try {
            prepare(i);
          } catch (...) {
            // Preparation is optional; the ordinary demand path owns recovery.
          }
        }
      };
      uthread::fork_join(workers, work, "recipe_partition_prepare");
    });
    return true;
  }

 private:
  const size_t endpoints_, partitions_, workers_;
  std::unique_ptr<std::atomic<bool>[]> started_;
};

}  // namespace starfish::recompute
