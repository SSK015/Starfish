#pragma once

#ifdef NO_REMOTE

#include "mm/cache/base/no_remote.hpp"  // IWYU pragma: export

#else

#include "mm/cache/accessor.hpp"  // IWYU pragma: export

#endif

#define __define_oms__(__scope) \
    FarLib::cache::OnMissScope oms(__entry__, &__scope);
