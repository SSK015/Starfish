// CPU-only end-to-end accounting/log wiring; no RDMA endpoint is started.
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <sstream>
#include <thread>

#if __has_include(<infiniband/verbs.h>)
#include "mm/allocator/remote_allocator.hpp"

// This allocator-only fixture does not enable phased RMW or link its RDMA
// coordinator. Fail if that unrelated diagnostic setup is ever requested.
namespace starfish::cache {
void prepare_ec_rmw_timing(size_t) {
  assert(false && "phased RMW timing is outside this accounting fixture");
}
}

namespace starfish {
static rdma::Configure test_config;
const rdma::Configure &get_config() { return test_config; }
}
namespace starfish::allocator::remote {
RemoteGlobalHeap remote_global_heap;
}

int main() {
  auto &c = starfish::test_config;
  c.server_count = 6;
  c.server_buffer_size = 4ull * 1024 * 1024;
  c.client_buffer_size = 4ull * 1024 * 1024;
  c.remote_total = c.server_count * c.server_buffer_size;
  c.mapping_type = starfish::rdma::Configure::MAPPING_RANGE;
  c.ft_method_type = starfish::rdma::Configure::FT_EC_BATCH;
  c.ft_method = "ec_batch";
  c.ft_small_object_cutoff = 4096;
  c.ft_small_stripe_shard_size_bytes =
      starfish::cache::SmallObjectStripeShardSize;
  c.ft_ec_data_shards = 4;
  c.ft_ec_parity_shards = 2;
  c.exclusive_cache = true;
  setenv("FARLIB_EC_SPACE_SAMPLE_MS", "100", 1);
  std::ostringstream captured;
  auto *old = std::cout.rdbuf(captured.rdbuf());
  {
    starfish::cache::RemoteAllocator allocator(c.remote_total);
    auto &manager = allocator.small_object_stripe_manager();
    auto &heap = starfish::allocator::remote::remote_global_heap;
    starfish::cache::SmallObjectStripeManager::SlotGroupHandle group;
    assert(manager.allocate_slot_group(4072, &group));
    const uint32_t sizes[4] = {4072, 4000, 3900, 3800};
    assert(manager.seal_slot_group(group.id, 0xf, sizes));
    assert(heap.get_used_bytes() == 6 * 4096);
    const auto ep = heap.get_endpoint_used_bytes();
    assert(ep.size() == 6);
    assert(std::accumulate(ep.begin(), ep.end(), uint64_t{0}) == 6 * 4096);
    for (size_t i = 0; i < ep.size(); ++i) {
      assert(ep[i] == 4096);
      assert(heap.get_server_used_bytes(i) == 4096);
    }
    assert(heap.get_committed_bytes() ==
           heap.get_ec_shard_region_count() *
               starfish::allocator::remote::RegionSize);
    assert(heap.get_committed_bytes() >=
           6 * starfish::cache::SmallObjectStripeShardSize);
    // Existing non-group accounting and the EC supplement must add, not
    // overwrite each other. Use a separate endpoint-mapped fixture address.
    const uint64_t legacy_addr = c.remote_total - 4096;
    heap.inc_used_bytes(128, legacy_addr);
    assert(heap.get_used_bytes() == 6 * 4096 + 128);
    const auto mixed = heap.get_endpoint_used_bytes();
    assert(std::accumulate(mixed.begin(), mixed.end(), uint64_t{0}) ==
           6 * 4096 + 128);
    heap.dec_used_bytes(128, legacy_addr);
    // A partial release must not shrink six-slot physical occupancy.
    assert(
        manager.release_group_object(group.segments[0].addr) ==
        starfish::cache::SmallObjectStripeManager::kGroupReleaseObjectReleased);
    assert(heap.get_used_bytes() == 6 * 4096);
    assert(starfish::profile::memory_usage_begin_observer);
    starfish::profile::memory_usage_begin_observer(
        starfish::profile::memory_usage_observer_context);
    std::this_thread::sleep_for(std::chrono::milliseconds(230));
    starfish::profile::memory_usage_end_observer(
        starfish::profile::memory_usage_observer_context);
    heap.print_used_memory();
    assert(manager.mark_dead_group(group.id));
    assert(heap.get_used_bytes() == 0);
    // Reserved backing remains reusable and must not be reported as live.
    assert(heap.get_committed_bytes() != 0);
  }
  std::cout.rdbuf(old);
  const auto log = captured.str();
  for (const auto *key :
       {"ec_space phase=work_begin", "ec_space phase=work_sample",
        "ec_space phase=work_end", "ec_space phase=allocator_destroy",
        "remote.exact_allocated_bytes: 24576",
        "small_groups_by_live=0:0,1:0,2:0,3:1,4:0",
        "work_sampled_peak_occupied_bytes=24576", "occupied_group_bytes=0",
        "sealed_space_amplification="}) {
    assert(log.find(key) != std::string::npos);
  }
  assert(starfish::profile::memory_usage_observer_context == nullptr);
  assert(starfish::profile::memory_usage_begin_observer == nullptr);
  assert(starfish::profile::memory_usage_end_observer == nullptr);
  // Teardown must not hide a leaked live group from the later heap exit log.
  // This fixture deliberately leaves one group allocated; no remote service
  // or real data is involved.
  setenv("FARLIB_EC_SPACE_SAMPLE_MS", "0", 1);
  {
    starfish::cache::RemoteAllocator allocator(c.remote_total);
    starfish::cache::SmallObjectStripeManager::SlotGroupHandle group;
    assert(allocator.small_object_stripe_manager().allocate_slot_group(4072,
                                                                       &group));
  }
  assert(starfish::allocator::remote::remote_global_heap.get_used_bytes() ==
         6 * 4096);
  starfish::allocator::remote::remote_global_heap.register_remote(
      c.remote_total);
  assert(starfish::allocator::remote::remote_global_heap.get_used_bytes() == 0);
  std::cout << log << "EC space accounting/reporting integration PASS\n";
}
#else
int main() {
  std::cerr << "SKIP: libibverbs headers unavailable\n";
  return 77;
}
#endif
