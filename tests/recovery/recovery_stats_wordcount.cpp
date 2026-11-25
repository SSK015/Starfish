#include "ft/recompute/recovery_stats.hpp"

#include <barrier>
#include <cassert>
#include <thread>

int main() {
    using Stats = starfish::recompute::RecoveryStats;
    Stats stats(true);
    assert(stats.snapshot().recovery_wall_ns == 0);
    std::barrier entered(3), release(3);
    auto worker = [&](bool ok) {
        Stats::Callback replay(stats);
        Stats::Build build(stats);
        entered.arrive_and_wait();
        release.arrive_and_wait();
        if (ok) build.success();
    };
    std::jthread first(worker, true), second(worker, false);
    {
        Stats::Batch prepare(stats);
        entered.arrive_and_wait();
        const auto live = stats.snapshot();
        assert(live.peak_builders == 2);
        assert(live.peak_callbacks == 2);
    }
    release.arrive_and_wait();
    first.join();
    second.join();
    const auto done = stats.snapshot();
    assert(done.preparation_batches == 1 && done.partition_build_attempts == 2);
    assert(done.rebuilt_partitions == 1 && done.failed_partition_builds == 1);
    assert(done.callbacks == 2 && done.recovery_wall_ns > 0);
    assert(done.recovery_wall_ns >= done.batch_wall_ns);
    assert(done.recovery_wall_ns >= done.callback_wall_ns);
    assert(done.callback_wall_ns >= done.build_wall_ns);
    {
        Stats::Callback later(stats);
    }
    assert(stats.snapshot().recovery_wall_ns > done.recovery_wall_ns);
}
