#include "data_structure/ordinal_table.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <vector>

namespace {

using starfish::recompute::OrdinalMode;

struct ConstantHash {
    size_t operator()(uint32_t) const noexcept { return 0; }
};

struct SpreadHash {
    size_t operator()(uint32_t key) const noexcept {
        return static_cast<size_t>(key * 2654435761u) % 4093;
    }
};

constexpr uint32_t entries = 4096;

uint32_t key_at(uint64_t id) {
    return static_cast<uint32_t>((37 * id + 11) % entries);
}

uint64_t expected_value(uint32_t key) {
    return key == 17 ? 991 : static_cast<uint64_t>(key) * 17 + 3;
}

template <class Hash>
void check_replay(size_t reserve, size_t resize_at, size_t new_reserve) {
    using Replay = starfish::recompute::OrdinalTable<
        uint32_t, uint64_t, Hash, OrdinalMode::replay>;
    Replay table;
    table.reserve(reserve);
    for (uint32_t id = 0; id < entries; ++id) {
        const uint32_t key = key_at(id);
        table[key] = static_cast<uint64_t>(key) * 17 + 3;
        if (id == resize_at) table.reserve(new_reserve);
    }
    // Rehash moved buckets but the index already refers to the original nodes.
    table[17] = 991;
    auto unexpected = [](uint32_t, uint64_t, uint64_t) {
        assert(false && "unavailable row was projected");
        return true;
    };
    assert(!table.visit_ordinal(0, unexpected));
    table.seal_replay();
    assert(table.size() == entries);

    std::vector<bool> seen(entries, false);
    assert(table.visit([&](uint32_t key, uint64_t value, uint64_t id) {
        assert(id < entries && !seen[id]);
        assert(key == key_at(id) && value == expected_value(key));
        seen[id] = true;
    }));
    for (bool visited : seen) assert(visited);
    for (uint64_t id = 0; id < entries; ++id) {
        assert(table.visit_ordinal(id, [id](uint32_t key, uint64_t value, uint64_t actual) {
            return actual == id && key == key_at(id) && value == expected_value(key);
        }));
    }
    assert(!table.visit_ordinal(entries, unexpected));
    assert(!table.visit_ordinal(std::numeric_limits<uint64_t>::max(), unexpected));
    assert(!table.visit_ordinal(0, [](uint32_t, uint64_t, uint64_t) { return false; }));
}

void test_overaligned_replay_values() {
    struct alignas(128) Counter { unsigned count = 0; };
    starfish::recompute::OrdinalTable<uint32_t, Counter, SpreadHash, OrdinalMode::replay> table;
    table.reserve(128);
    for (uint32_t k = 0; k < 128; ++k) ++table[k].count;
    table.seal_replay();
    for (uint64_t id = 0; id < 128; ++id) {
        assert(table.visit_ordinal(id, [id](auto key, const Counter &value, auto) {
            return key == id && value.count == 1 &&
                   reinterpret_cast<uintptr_t>(&value) % alignof(Counter) == 0;
        }));
    }
}

void test_ordinary_table_has_no_replay_index_requirement() {
    using Table = starfish::recompute::OrdinalTable<
        uint32_t, uint64_t, ConstantHash, OrdinalMode::plain>;
    static_assert(!Table::tracks_ordinals);
    Table table;
    table.reserve(64);
    for (uint32_t key = 0; key < 64; ++key) table[key] = key + 1;
    table[7] = 100;

    assert(table.size() == 64);
    uint64_t total = 0;
    table.visit([&](const uint32_t &, const uint64_t &value, uint64_t) {
        total += value;
    });
    assert(total == (64 * 65) / 2 - 8 + 100);
}

template <OrdinalMode Mode>
void test_iteration() {
    using Table = starfish::recompute::OrdinalTable<uint32_t, uint64_t, ConstantHash, Mode>;
    static_assert(std::ranges::forward_range<Table> && std::ranges::forward_range<const Table>);
    Table table;
    table[1] = 8;
    table[2] = 9;
    if constexpr (Mode == OrdinalMode::replay)
        table.seal_replay();
    else
        for (auto [key, value] : table) value += key;
    uint64_t sum = 0;
    for (const auto &[key, value] : std::as_const(table)) sum += value;
    assert(sum == (Mode == OrdinalMode::replay ? 17 : 20));
    auto i = table.begin();
    typename Table::const_iterator first = i++;
    assert(first == table.cbegin() && first->first != i->first);
    assert(std::ranges::distance(table) == 2);
}

}  // namespace

int main() {
    check_replay<ConstantHash>(8, 31, 8192);
    check_replay<SpreadHash>(2, 777, 16384);
    test_iteration<OrdinalMode::plain>();
    test_iteration<OrdinalMode::tracked>();
    test_iteration<OrdinalMode::replay>();
    test_overaligned_replay_values();
    test_ordinary_table_has_no_replay_index_requirement();
}
