#pragma once
#include "transport/rdma/config.hpp"

namespace starfish {
void runtime_init(const rdma::Configure& config, bool enable_cache = true);
void runtime_quiesce_cache();
void runtime_destroy();
const rdma::Configure& get_config();
}  // namespace starfish