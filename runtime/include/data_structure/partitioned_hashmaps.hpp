#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "ft/recompute/partition_recipes.hpp"
#include "lib/control.hpp"
#include "mm/cache/base/scope.hpp"

namespace starfish {

// Maps live until their ordinary owners release them. Each bound output keeps
// its immutable replay state alive independently of this collection. Construct
// the collection after runtime_init; maps only weakly reference its job state.
// Recipe annotations are optional per partition; unmarked maps publish normally.
template <class Map>
class PartitionedHashMaps {
 public:
  PartitionedHashMaps(size_t count, uint32_t shift) {
    const auto &config = starfish::get_config();
    // Unsupported modes remain ordinary until an explicit annotation, which
    // reports misuse. The job's recovery registry is allocated only on bind.
    if (count != 0 && config.supports_partition_recipes()) {
      recipes_ = std::make_shared<recompute::PartitionRecipes>(
          static_cast<size_t>(config.server_count), count,
          uthread::get_application_worker_count());
    }
    maps_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      maps_.push_back(std::make_shared<Map>(shift));
      if (recipes_) maps_.back()->enable_partition_recipes(recipes_, i);
    }
  }

  std::shared_ptr<Map> &operator[](size_t i) { return maps_.at(i); }

  template <class Table, class Scope>
  bool publish(size_t i, const Table &rows, Scope &scope) {
    const auto map = maps_.at(i);
    if (!map) throw std::logic_error("partition output map has been released");
    return map->publish_partition(rows, scope);
  }

 private:
  std::vector<std::shared_ptr<Map>> maps_;
  std::shared_ptr<recompute::PartitionRecipes> recipes_;
};

namespace cache {

template <class Table, class Output, class Input, class Producer>
bool DereferenceScope::mark_recomputable(Output &output, Input input,
                                         Producer producer) {
  using Map = typename Output::element_type;
  using RecipeTable = std::conditional_t<std::is_void_v<Table>,
                                         typename Map::DefaultRecipeTable, Table>;
  if (!output) throw std::invalid_argument("partition recipe output is null");
  if constexpr (requires { requires RecipeTable::tracks_ordinals; }) {
    return output->template arm_partition_recipe<RecipeTable>(
        output, std::move(input), std::move(producer));
  } else {
    throw std::invalid_argument("partition recipes require first-insertion ordinals");
  }
}

}  // namespace cache

}  // namespace starfish
