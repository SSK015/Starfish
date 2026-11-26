#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "graph_progress.hpp"

namespace graph_input {
namespace fs = std::filesystem;

constexpr std::size_t kMaxLoaderThreads = 32;

struct Edge {
    std::uint64_t source;
    std::uint64_t destination;
};

struct ShardData {
    std::set<std::uint64_t> vertices;
    std::vector<Edge> edges;
};

class AtomicFlagGuard {
public:
    explicit AtomicFlagGuard(std::atomic_flag &flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {}
    }
    ~AtomicFlagGuard() { flag_.clear(std::memory_order_release); }
    AtomicFlagGuard(const AtomicFlagGuard &) = delete;
    AtomicFlagGuard &operator=(const AtomicFlagGuard &) = delete;

private:
    std::atomic_flag &flag_;
};

[[noreturn]] inline void fail(const fs::path &path, std::uint64_t line,
                              const std::string &reason) {
    std::ostringstream message;
    message << "Graph input error at " << path.string();
    if (line != 0) message << ':' << line;
    message << ": " << reason;
    throw std::runtime_error(message.str());
}

inline bool ascii_whitespace(char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

inline std::uint64_t parse_decimal(const std::string &text, std::size_t &position,
                                   const fs::path &path, std::uint64_t line,
                                   const char *field) {
    const std::size_t begin = position;
    std::uint64_t value = 0;
    while (position < text.size() && text[position] >= '0' &&
           text[position] <= '9') {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(text[position] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            fail(path, line, std::string(field) + " exceeds uint64");
        value = value * 10 + digit;
        ++position;
    }
    if (position == begin)
        fail(path, line, std::string("expected unsigned decimal ") + field);
    return value;
}

inline void skip_whitespace(const std::string &line, std::size_t &position) {
    while (position < line.size() && ascii_whitespace(line[position])) ++position;
}

inline bool parse_edge_line(const std::string &line, const fs::path &path,
                            std::uint64_t line_number, Edge &edge) {
    std::size_t position = 0;
    skip_whitespace(line, position);
    if (position == line.size() || line[position] == '#') return false;

    edge.source = parse_decimal(line, position, path, line_number, "source ID");
    if (position == line.size() || !ascii_whitespace(line[position]))
        fail(path, line_number, "source ID must be followed by ASCII whitespace");
    skip_whitespace(line, position);
    if (position == line.size() || line[position] == '#')
        fail(path, line_number, "missing destination ID");

    edge.destination =
        parse_decimal(line, position, path, line_number, "destination ID");
    if (position < line.size() && line[position] == '#') return true;
    if (position < line.size() && !ascii_whitespace(line[position]))
        fail(path, line_number,
             "destination ID must be followed by whitespace or a comment");
    skip_whitespace(line, position);
    if (position < line.size() && line[position] != '#')
        fail(path, line_number, "unexpected additional column");
    return true;
}

inline std::uint64_t parse_index(const std::string &digits,
                                const fs::path &path) {
    if (digits.empty()) fail(path, 0, "empty numbered-shard suffix");
    std::size_t position = 0;
    const std::uint64_t index = parse_decimal(digits, position, path, 0,
                                              "shard index");
    if (position != digits.size())
        fail(path, 0, "numbered shard suffix must contain decimal digits only");
    if (std::to_string(index) != digits)
        fail(path, 0, "numbered shard filename is not canonical");
    return index;
}

inline std::uint64_t read_shard_count(const fs::path &count_path) {
    std::error_code error;
    const fs::file_status status = fs::status(count_path, error);
    if (error || !fs::is_regular_file(status))
        fail(count_path, 0, "shard-count is missing or is not a regular file");

    std::ifstream input(count_path, std::ios::binary);
    if (!input.is_open()) fail(count_path, 0, "cannot open shard-count");
    char buffer[23]; // A uint64 has at most 20 digits, plus an optional CRLF.
    input.read(buffer, sizeof(buffer));
    if (input.gcount() == sizeof(buffer))
        fail(count_path, 0, "shard-count is too long");
    std::string text(buffer, static_cast<std::size_t>(input.gcount()));
    if (input.bad()) fail(count_path, 0, "failed while reading shard-count");
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0)
        text.resize(text.size() - 2);
    else if (!text.empty() && text.back() == '\n')
        text.pop_back();
    if (text.empty()) fail(count_path, 0, "shard-count must be a positive decimal");

    std::size_t position = 0;
    const std::uint64_t count =
        parse_decimal(text, position, count_path, 0, "shard count");
    if (position != text.size())
        fail(count_path, 0,
             "shard-count must contain one positive decimal and an optional line ending");
    if (count == 0) fail(count_path, 0, "shard-count must be positive");
    return count;
}

inline void validate_shard_sequence(
    const std::map<std::uint64_t, fs::path> &found, std::uint64_t expected,
    const fs::path &root, const std::string &filename_prefix) {
    std::uint64_t next = 0;
    for (const auto &[index, path] : found) {
        if (index >= expected)
            fail(path, 0, "unexpected extra numbered shard; expected " +
                                std::to_string(expected) + " shards");
        if (index != next)
            fail(root / (filename_prefix + std::to_string(next)), 0,
                 "missing numbered shard");
        ++next;
    }
    if (next < expected)
        fail(root / (filename_prefix + std::to_string(next)), 0,
             "missing numbered shard");
}

inline void require_regular_file(const fs::path &path) {
    std::error_code error;
    const fs::file_status status = fs::status(path, error);
    if (error || !fs::is_regular_file(status))
        fail(path, 0, "graph shard is missing or is not a regular file");
}

inline std::map<std::uint64_t, fs::path> find_numbered_files(
    const fs::path &directory, const std::string &prefix,
    const fs::path &context, bool reject_non_numeric) {
    std::map<std::uint64_t, fs::path> found;
    std::error_code error;
    fs::directory_iterator iterator(directory, error), end;
    if (error)
        fail(context, 0, "cannot inspect shard directory: " + error.message());
    for (; iterator != end; iterator.increment(error)) {
        if (error)
            fail(context, 0, "cannot inspect shard directory: " + error.message());
        const fs::path entry_path = iterator->path();
        const std::string name = entry_path.filename().string();
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string suffix = name.substr(prefix.size());
        const bool decimal = !suffix.empty() &&
            std::all_of(suffix.begin(), suffix.end(),
                        [](char c) { return c >= '0' && c <= '9'; });
        if (!decimal) {
            if (reject_non_numeric)
                fail(entry_path, 0, "numbered shard suffix must contain decimal digits");
            continue;
        }
        const std::uint64_t index = parse_index(suffix, entry_path);
        require_regular_file(entry_path);
        if (!found.emplace(index, entry_path).second)
            fail(entry_path, 0, "duplicate numbered shard index");
    }
    if (error)
        fail(context, 0, "cannot inspect shard directory: " + error.message());
    return found;
}

inline std::vector<fs::path> paths_from_shards(
    const std::map<std::uint64_t, fs::path> &found) {
    std::vector<fs::path> paths;
    paths.reserve(found.size());
    for (const auto &[index, path] : found) {
        (void)index;
        paths.push_back(path);
    }
    return paths;
}

inline std::vector<fs::path> resolve_directory(const fs::path &directory) {
    const std::uint64_t expected = read_shard_count(directory / "shard-count");
    const auto found = find_numbered_files(directory, "edges.", directory, true);
    validate_shard_sequence(found, expected, directory, "edges.");
    return paths_from_shards(found);
}

inline std::vector<fs::path> resolve_legacy_prefix(const fs::path &base_path) {
    const fs::path parent = base_path.parent_path().empty()
                                ? fs::path(".")
                                : base_path.parent_path();
    const std::string prefix = base_path.filename().string() + ".";
    const auto found =
        find_numbered_files(parent, prefix, base_path, false);
    if (found.empty())
        fail(base_path, 0,
             "input is absent; no regular file, directory, or legacy .0-.31 shards found");
    constexpr std::uint64_t legacy_shard_count = 32;
    validate_shard_sequence(found, legacy_shard_count, parent, prefix);
    return paths_from_shards(found);
}

inline std::vector<fs::path> resolve_input(const fs::path &input_path) {
    std::error_code error;
    const fs::file_status status = fs::status(input_path, error);
    if (error && error != std::errc::no_such_file_or_directory &&
        error != std::errc::not_a_directory)
        fail(input_path, 0, "cannot inspect input path: " + error.message());
    if (!error && fs::is_regular_file(status)) return {input_path};
    if (!error && fs::is_directory(status)) return resolve_directory(input_path);
    if (!error && fs::exists(status))
        fail(input_path, 0, "input path must be a regular file or directory");
    return resolve_legacy_prefix(input_path);
}

inline void read_edge_file(const fs::path &path, ShardData &data) {
    require_regular_file(path);
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) fail(path, 0, "cannot open graph shard");
    std::string line;
    std::uint64_t line_number = 0;
    while (std::getline(input, line)) {
        if (line_number == std::numeric_limits<std::uint64_t>::max())
            fail(path, line_number, "line number exceeds uint64");
        ++line_number;
        Edge edge{};
        if (!parse_edge_line(line, path, line_number, edge)) continue;
        data.vertices.insert(edge.source);
        data.vertices.insert(edge.destination);
        data.edges.push_back(edge);
    }
    if (input.bad()) fail(path, line_number + 1, "I/O error while reading graph shard");
}

inline int exception_error_code(std::exception_ptr error) noexcept {
    try {
        if (error) std::rethrow_exception(error);
    } catch (const std::system_error &system_error) {
        return system_error.code().value();
    } catch (...) {
        return EINVAL;
    }
    return 0;
}

template <typename Affinity, typename Work, typename Progress>
void run_shard_workers(std::size_t shard_count, const char *phase,
                       const Affinity &affinity, Work work,
                       Progress progress) {
    const std::size_t worker_count =
        std::min(shard_count, kMaxLoaderThreads);
    if (worker_count == 0) throw std::runtime_error("no graph shards to load");

    std::atomic<std::size_t> next_shard{worker_count};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> affinity_applied{0};
    std::atomic<int> affinity_error{0};
    std::atomic<bool> cancelled{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    auto capture_failure = [&](std::exception_ptr current) {
        std::lock_guard<std::mutex> guard(failure_mutex);
        if (!failure) failure = current;
        cancelled.store(true, std::memory_order_relaxed);
    };
    std::exception_ptr launch_failure;
    try {
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.emplace_back([&, worker] {
                bool affinity_ready = false;
                try {
                    for (std::size_t index = worker;
                         index < shard_count && !cancelled.load(std::memory_order_relaxed);
                         index = next_shard.fetch_add(1, std::memory_order_relaxed)) {
                        try {
                            affinity.apply(phase, index, !affinity_ready);
                        } catch (...) {
                            int expected = 0;
                            affinity_error.compare_exchange_strong(
                                expected, exception_error_code(std::current_exception()));
                            throw;
                        }
                        if (!affinity_ready) {
                            affinity_ready = true;
                            affinity_applied.fetch_add(1, std::memory_order_relaxed);
                        }
                        work(index);
                        const std::size_t done =
                            completed.fetch_add(1, std::memory_order_relaxed) + 1;
                        progress(done, index);
                    }
                } catch (...) {
                    capture_failure(std::current_exception());
                }
            });
        }
    } catch (...) {
        launch_failure = std::current_exception();
        cancelled.store(true, std::memory_order_relaxed);
    }
    for (std::thread &worker : workers) {
        if (worker.joinable()) worker.join();
    }

    affinity.report_phase(phase, worker_count,
                          affinity_applied.load(std::memory_order_relaxed),
                          affinity_error.load(std::memory_order_relaxed));
    if (launch_failure) std::rethrow_exception(launch_failure);
    if (failure) std::rethrow_exception(failure);
}

template <typename Affinity>
bool read_graph(const std::string &input_name,
                std::vector<std::vector<std::uint64_t>> &adjacency,
                std::uint64_t &vertex_count, const Affinity &affinity) {
    const auto start = std::chrono::steady_clock::now();
    const auto progress = [&](const char *phase, const std::string &detail) {
        print_graph_progress(phase, start, detail);
    };
    const fs::path input_path(input_name);
    adjacency.clear();
    vertex_count = 0;
    try {
        const std::vector<fs::path> shards = resolve_input(input_path);
        progress("start", "files=" + std::to_string(shards.size()) +
                               " base=" + input_name);

        std::vector<ShardData> shard_data(shards.size());
        std::mutex progress_mutex;
        run_shard_workers(
            shards.size(), "read", affinity,
            [&](std::size_t index) {
                read_edge_file(shards[index], shard_data[index]);
            },
            [&](std::size_t done, std::size_t index) {
                std::lock_guard<std::mutex> guard(progress_mutex);
                progress("read_shard_done",
                         "shard=" + std::to_string(index) +
                             " completed=" + std::to_string(done) +
                             " shard_edges=" +
                             std::to_string(shard_data[index].edges.size()) +
                             " shard_vertices=" +
                             std::to_string(shard_data[index].vertices.size()));
            });

        std::uint64_t total_edges = 0;
        for (const ShardData &data : shard_data) {
            if (data.edges.size() >
                std::numeric_limits<std::uint64_t>::max() - total_edges)
                fail(input_path, 0, "total edge count exceeds uint64");
            total_edges += static_cast<std::uint64_t>(data.edges.size());
        }
        if (total_edges == 0) fail(input_path, 0, "graph contains no edges");
        if (total_edges > std::numeric_limits<std::uint64_t>::max() / 2)
            fail(input_path, 0, "undirected adjacency count exceeds uint64");
        progress("read_shards_joined", "input_edges=" +
                                             std::to_string(total_edges));

        std::set<std::uint64_t> all_vertices;
        for (std::size_t index = 0; index < shard_data.size(); ++index) {
            all_vertices.insert(shard_data[index].vertices.begin(),
                                shard_data[index].vertices.end());
            if ((index + 1) % 4 == 0 || index + 1 == shard_data.size())
                progress("merge_vertices",
                         "merged_shards=" + std::to_string(index + 1) +
                             " vertices_so_far=" +
                             std::to_string(all_vertices.size()));
        }
        if (all_vertices.size() >=
            static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()))
            fail(input_path, 0, "vertex count cannot accommodate sentinel list");
        vertex_count = static_cast<std::uint64_t>(all_vertices.size());
        progress("vertex_map_done", "vertices=" + std::to_string(vertex_count));

        std::unordered_map<std::uint64_t, std::uint64_t> vertex_map;
        vertex_map.reserve(all_vertices.size());
        std::uint64_t dense_id = 0;
        for (const std::uint64_t vertex : all_vertices)
            vertex_map.emplace(vertex, dense_id++);

        adjacency.resize(all_vertices.size() + 1);
        progress("adjacency_resize_done",
                 "adjacency_lists=" + std::to_string(adjacency.size()));
        std::cout << "Creating locks" << std::endl;
        std::vector<std::atomic_flag> locks(adjacency.size());
        for (std::atomic_flag &lock : locks) lock.clear();
        progress("locks_ready", "locks=" + std::to_string(locks.size()));

        run_shard_workers(
            shards.size(), "fill", affinity,
            [&](std::size_t index) {
                for (const Edge &edge : shard_data[index].edges) {
                    const std::uint64_t source = vertex_map.at(edge.source);
                    const std::uint64_t destination = vertex_map.at(edge.destination);
                    {
                        AtomicFlagGuard guard(locks[source]);
                        adjacency[source].push_back(destination);
                    }
                    {
                        AtomicFlagGuard guard(locks[destination]);
                        adjacency[destination].push_back(source);
                    }
                }
            },
            [&](std::size_t done, std::size_t index) {
                std::lock_guard<std::mutex> guard(progress_mutex);
                progress("fill_shard_done",
                         "shard=" + std::to_string(index) +
                             " completed=" + std::to_string(done) +
                             " shard_edges=" +
                             std::to_string(shard_data[index].edges.size()));
            });

        progress("fill_adjacency_done",
                 "undirected_edges=" + std::to_string(total_edges * 2));
        std::cout << "Read graph done" << std::endl;
        return true;
    } catch (const std::exception &error) {
        adjacency.clear();
        vertex_count = 0;
        std::cerr << "graph_input_error " << error.what() << std::endl;
        return false;
    } catch (...) {
        adjacency.clear();
        vertex_count = 0;
        std::cerr << "graph_input_error unknown graph loader failure for "
                  << input_name << std::endl;
        return false;
    }
}

}  // namespace graph_input
