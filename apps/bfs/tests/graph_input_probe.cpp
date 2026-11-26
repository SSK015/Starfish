#include "graph_load_affinity.hpp"
#include "../../graph_input/graph_loader.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: graph_input_probe INPUT\n";
        return 2;
    }

    std::vector<std::vector<uint64_t>> adjacency;
    uint64_t vertices = 0;
    const GraphLoadAffinity affinity;
    if (!graph_input::read_graph(argv[1], adjacency, vertices, affinity)) {
        std::cerr << "loader rejected input: " << argv[1] << '\n';
        return 1;
    }

    const std::vector<std::vector<uint64_t>> expected{
        {1, 1, 3}, {0, 0, 3}, {2, 2}, {0, 1}, {}};
    auto sorted_adjacency = adjacency;
    for (auto &row : sorted_adjacency) {
        std::sort(row.begin(), row.end());
    }
    if (vertices != 4 || sorted_adjacency != expected) {
        std::cerr << "loader result mismatch for " << argv[1]
                  << ": vertices=" << vertices
                  << ", adjacency rows=" << adjacency.size() << '\n';
        return 1;
    }

    std::cout << "GRAPH_INPUT_PASS\n";
    return 0;
}
