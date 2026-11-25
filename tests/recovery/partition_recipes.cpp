#include "ft/recompute/partition_recipes.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>

int main() {
    namespace rt = starfish::recompute;
    namespace fiber = starfish::uthread;
    fiber::runtime_init(2);
    rt::Inputs outputs;
    std::weak_ptr<const int> input_lifetime;
    std::atomic<unsigned> builders{0}, requests{0};

    {
        rt::PartitionRecipes groups(2, 1, 2);
        auto input = std::make_shared<const int>(19);
        input_lifetime = input;
        // The immutable input and its callback outlive the registration scope.
        outputs = groups.bind(0, input,
            [&](const auto& value) {
                ++builders;
                while (requests.load() < 2) fiber::yield();
                return std::make_unique<int>(*value * 2);
            },
            [](size_t) { return false; },
            [](const auto&, const int& prepared, uint64_t locator,
               void* destination, size_t bytes) {
                if (bytes != sizeof(int)) return false;
                const int value = prepared + static_cast<int>(locator);
                std::memcpy(destination, &value, sizeof(value));
                return true;
            });
        assert(outputs.valid());
    }

    auto* plan = outputs.input_state()->plan_for(&rt::PartitionRecipes::replay);
    assert(plan && !input_lifetime.expired());
    int result[2]{};
    fiber::fork_join(2, [&](size_t i) {
        ++requests;
        assert(plan->invoke(&result[i], sizeof(int), i));
    }, "test_concurrent_recipe");
    assert(builders == 1 && result[0] == 38 && result[1] == 39);
    outputs.reset();
    assert(input_lifetime.expired());
    fiber::runtime_destroy();
}
