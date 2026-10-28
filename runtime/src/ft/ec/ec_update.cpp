// Keep the transaction coordinator out of the large application/cache header.
// It runs once per update batch; applications do not need to instantiate it.
#include "mm/cache/cache.hpp"
#include "ft/ec/ec_update_path.ipp"
#include "ft/ec/ec_rmw_path.ipp"
