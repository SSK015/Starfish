#pragma once

#include <cstddef>
#include <cstdint>

#include "profiling/benchmark_phase.hpp"

// Compatibility surface for the two shared benchmark drivers. The former
// request-level observer (sampling ring, locks, and syscall/TSC probes) is
// retired; stage state is kept in benchmark_phase for the optional EC-RMW
// timing extension.
namespace starfish::request_interval_diag {

inline void begin_stage(std::size_t fibres) noexcept {
  ::starfish::benchmark_phase::begin_stage(fibres);
}

inline void end_stage() noexcept { ::starfish::benchmark_phase::end_stage(); }

// Retain the shared-app call shape; request-level consumer tracing is gone.
inline void consumer_end(std::size_t, uint64_t) noexcept {}
inline void dump() noexcept {}

}  // namespace starfish::request_interval_diag
