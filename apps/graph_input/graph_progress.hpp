#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace graph_input {

inline bool graph_progress_enabled() {
    const char *env = std::getenv("NHOP_GRAPH_PROGRESS");
    if (env == nullptr || env[0] == '\0') {
        return true;
    }
    return std::strtoull(env, nullptr, 0) != 0;
}

inline uint64_t graph_current_rss_bytes() {
    std::ifstream statm("/proc/self/statm");
    uint64_t total_pages = 0;
    uint64_t resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return 0;
    }
    return resident_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

inline double graph_elapsed_s(
    const std::chrono::steady_clock::time_point &start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
}

inline void print_graph_progress(
    const char *phase, const std::chrono::steady_clock::time_point &start,
    const std::string &extra = "") {
    if (!graph_progress_enabled()) {
        return;
    }
    std::cout << "graph_load_progress"
              << " phase=" << phase
              << " elapsed_s=" << graph_elapsed_s(start)
              << " rss_bytes=" << graph_current_rss_bytes();
    if (!extra.empty()) {
        std::cout << " " << extra;
    }
    std::cout << std::endl;
}

} // namespace graph_input
