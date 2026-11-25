#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_structure/partitioned_hashmaps.hpp"
#include "wordcount_common.hpp"

namespace starfish::wordcount {

struct PhaseStats {
    int64_t rdma_read_bytes;
    int64_t rdma_write_bytes;
};

inline size_t env_size(const char *name, size_t fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    return std::strtoull(value, nullptr, 0);
}

inline size_t parse_size_arg(const char *text, const char *name) {
    char *end = nullptr;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        throw std::runtime_error(std::string("invalid ") + name + ": " + text);
    }
    return static_cast<size_t>(value);
}

inline PhaseStats collect_phase_stats() {
    return {starfish::profile::collect_rdma_read_post_bytes(),
            starfish::profile::collect_rdma_write_post_bytes()};
}

inline PhaseStats operator-(const PhaseStats &after, const PhaseStats &before) {
    return {after.rdma_read_bytes - before.rdma_read_bytes,
            after.rdma_write_bytes - before.rdma_write_bytes};
}

inline void print_phase_result(const char *target_name, const char *phase,
                               double elapsed_s, size_t ops,
                               const PhaseStats &delta) {
    std::cout << target_name << "_phase phase=" << phase
              << " elapsed_s=" << elapsed_s
              << " ops=" << ops
              << " rdma_read_bytes=" << delta.rdma_read_bytes
              << " rdma_write_bytes=" << delta.rdma_write_bytes
              << "\n";
}

inline void print_usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " <config> <dataset> [threads] [max_bytes] [top_k]\n";
}

template <bool TrackRecipes, class PartitionCallback>
int run_wordcount_job(int argc, char **argv,
                      PartitionCallback &&process_partition) {
    std::cout.setf(std::ios::unitbuf);
    if (argc < 3 || argc > 6) {
        print_usage(argv[0]);
        return 2;
    }

    try {
        starfish::rdma::Configure config;
        config.from_file(argv[1]);

        const char *target_name = TrackRecipes ? "wordcount_recipes" : "wordcount_far";
        if constexpr (TrackRecipes) {
            config.require_partition_recipe_support();
        }

        size_t threads = std::min<size_t>(
            env_size("FARLIB_WORDCOUNT_THREADS", 24), config.max_thread_cnt);
        if (argc >= 4) {
            threads = std::min(parse_size_arg(argv[3], "threads"),
                               static_cast<size_t>(config.max_thread_cnt));
        }
        size_t max_bytes = 0;
        if (argc >= 5) {
            max_bytes = parse_size_arg(argv[4], "max_bytes");
        }
        size_t top_k = 20;
        if (argc >= 6) {
            top_k = parse_size_arg(argv[5], "top_k");
        }
        size_t global_map_shift = env_size("FARLIB_WORDCOUNT_GLOBAL_MAP_SHIFT", 25);
        size_t local_map_shift = env_size("FARLIB_WORDCOUNT_LOCAL_MAP_SHIFT", 21);
        size_t map_uthreads =
            std::max(env_size("FARLIB_WORDCOUNT_UTHREADS", threads), threads);
        ASSERT(threads > 0);
        auto file = std::make_shared<MappedFile>(argv[2]);
        size_t size = file->size;
        if (max_bytes != 0) {
            size = std::min(size, max_bytes);
        }
        if (size == 0) {
            // Keep the regular zero-result/cleanup path, but avoid dividing by
            // zero and avoid constructing a batch of empty native partitions.
            map_uthreads = 1;
        }
        auto chunks = make_chunks(file->data, size, threads);
        size_t subtasks_per_chunk =
            (map_uthreads + chunks.size() - 1) / chunks.size();
        size_t map_task_count = chunks.size() * subtasks_per_chunk;

        std::cout << target_name << " start file=" << argv[2]
                  << " bytes=" << size
                  << " threads=" << chunks.size()
                  << " requested_map_uthreads=" << map_uthreads
                  << " map_task_count=" << map_task_count
                  << " subtasks_per_chunk=" << subtasks_per_chunk
                  << " global_map_shift=" << global_map_shift
                  << " local_map_shift=" << local_map_shift
                  << " sizeof_key=" << sizeof(FixedWord)
                  << " sizeof_value=" << sizeof(CountValue)
                  << "\n";
        starfish::runtime_init(config);
        {
            starfish::PartitionedHashMaps<FarWordMap> local_maps(
                map_task_count, local_map_shift);
            FarWordMap global_map(global_map_shift);
            std::array<starfish::Spinlock, kGlobalLockCount> global_locks;
            std::vector<ThreadStats> task_stats(map_task_count);
            using NativeCountMap = FarWordMap::PublicationTable<TrackRecipes>;
            std::vector<std::unique_ptr<NativeCountMap>>
                native_partials(map_task_count);
            auto count_partition = [file](auto &rows, Chunk chunk) {
                return count_chunk_native(rows, file->data, chunk);
            };

            auto preagg_stats_before = collect_phase_stats();
            auto preagg_start = std::chrono::steady_clock::now();
            starfish::uthread::parallel_for_with_scope<1>(
                map_task_count, map_task_count,
                [&](size_t task_id, starfish::DereferenceScope &scope) {
                    size_t chunk_id = task_id % chunks.size();
                    size_t subtask_id = task_id / chunks.size();
                    Chunk parent = chunks[chunk_id];
                    size_t begin =
                        parent.begin +
                        (parent.end - parent.begin) * subtask_id /
                            subtasks_per_chunk;
                    size_t end =
                        parent.begin +
                        (parent.end - parent.begin) * (subtask_id + 1) /
                            subtasks_per_chunk;
                    if (subtask_id != 0) {
                        begin = advance_to_delimiter(file->data, begin,
                                                     parent.end);
                    }
                    if (subtask_id + 1 != subtasks_per_chunk) {
                        end = advance_to_delimiter(file->data, end,
                                                   parent.end);
                    }
                    if (begin > parent.end) {
                        begin = parent.end;
                    }
                    if (end > parent.end) {
                        end = parent.end;
                    }
                    Chunk subchunk{begin, end};
                    auto native_counts = std::make_unique<NativeCountMap>();
                    if (subchunk.begin != subchunk.end) {
                        native_counts->reserve(1 << 20);
                    }
                    task_stats[task_id] = process_partition(
                        scope, local_maps, task_id, subchunk,
                        *native_counts, count_partition);
                    native_partials[task_id] = std::move(native_counts);
                });
            auto preagg_stop = std::chrono::steady_clock::now();
            double preagg_elapsed =
                std::chrono::duration<double>(preagg_stop - preagg_start)
                    .count();
            uint64_t total_words = 0;
            uint64_t too_long_words = 0;
            uint64_t max_word_len = 0;
            size_t total_native_unique = 0;
            for (const auto &stat : task_stats) {
                total_words += stat.total_words;
                too_long_words += stat.too_long_words;
                max_word_len = std::max(max_word_len, stat.max_word_len);
                total_native_unique += stat.native_unique_words;
            }
            print_phase_result(target_name, "native_preagg",
                               preagg_elapsed, total_words,
                               collect_phase_stats() - preagg_stats_before);

            // Measure only the remote map/merge/reduce portion. Keep input
            // parsing and native preaggregation outside the planner window.
            starfish::profile::start_work();
            std::cout << target_name << "_work_phase_start phase=map_local\n";
            auto map_stats_before = collect_phase_stats();
            auto map_start = std::chrono::steady_clock::now();
            auto map_one_task = [&](size_t task_id, auto &scope) {
                auto &native_counts = native_partials[task_id];
                ASSERT(local_maps.publish(task_id, *native_counts, scope));
                native_counts.reset();
            };
            starfish::uthread::parallel_for_with_scope<1>(
                map_task_count, map_task_count, map_one_task);
            auto map_stop = std::chrono::steady_clock::now();
            double map_elapsed =
                std::chrono::duration<double>(map_stop - map_start).count();
            print_phase_result(target_name, "map_local", map_elapsed,
                               total_native_unique,
                               collect_phase_stats() - map_stats_before);

            auto merge_stats_before = collect_phase_stats();
            auto merge_start = std::chrono::steady_clock::now();
            std::atomic_size_t merged_unique{0};
            size_t merge_worker_count =
                env_size("FARLIB_WORDCOUNT_MERGE_WORKERS",
                         std::min<size_t>(threads, map_task_count));
            merge_worker_count = std::max<size_t>(
                1, std::min<size_t>(merge_worker_count, map_task_count));
            starfish::uthread::parallel_for_with_scope<1, true>(
                merge_worker_count, map_task_count,
                [&](size_t tid, auto &scope) {
                    local_maps[tid]->for_each_snapshot(
                        [&](const FixedWord &word, const CountValue &value,
                            auto &inner_scope) {
                            size_t lock_idx =
                                FixedWordHash{}(word) &
                                (kGlobalLockCount - 1);
                            // Merge uses copied rows; release the read epoch while the lock owner allocates.
                            global_locks[lock_idx].lock(inner_scope, true);
                            add_to_map(global_map, word, value.count,
                                       inner_scope);
                            global_locks[lock_idx].unlock();
                            ++merged_unique;
                        },
                        scope);
                    local_maps[tid].reset();
                });
            auto merge_stop = std::chrono::steady_clock::now();
            double merge_elapsed =
                std::chrono::duration<double>(merge_stop - merge_start).count();
            print_phase_result(target_name, "merge_global",
                               merge_elapsed, merged_unique.load(),
                               collect_phase_stats() - merge_stats_before);

            auto reduce_stats_before = collect_phase_stats();
            auto reduce_start = std::chrono::steady_clock::now();
            std::unordered_map<std::string, uint64_t> native_counts;
            native_counts.reserve(global_map.size.load());
            const size_t reduce_readers =
                env_size("FARLIB_WORDCOUNT_REDUCE_READERS", 1);
            if (reduce_readers > 1) {
                global_map.for_each_readonly_parallel(
                    [&](const FixedWord &word, const CountValue &value) {
                        native_counts.emplace(std::string(word.view()),
                                              value.count);
                    },
                    reduce_readers);
            } else {
                starfish::RootDereferenceScope scope;
                global_map.for_each_locked(
                    [&](const FixedWord &word, const CountValue &value,
                        auto &) {
                        native_counts.emplace(std::string(word.view()),
                                              value.count);
                    },
                    scope);
            }
            std::vector<std::pair<std::string, uint64_t>> sorted;
            sorted.reserve(native_counts.size());
            for (auto &entry : native_counts) {
                sorted.emplace_back(entry.first, entry.second);
            }
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto &a, const auto &b) {
                          return a.first < b.first;
                      });
            uint64_t checksum = checksum_counts(sorted);

            std::vector<std::pair<std::string_view, uint64_t>> top;
            top.reserve(sorted.size());
            for (const auto &entry : sorted) {
                top.emplace_back(std::string_view(entry.first), entry.second);
            }
            std::sort(top.begin(), top.end(),
                      [](const auto &a, const auto &b) {
                          if (a.second != b.second) {
                              return a.second > b.second;
                          }
                          return a.first < b.first;
                      });
            auto reduce_stop = std::chrono::steady_clock::now();
            double reduce_elapsed =
                std::chrono::duration<double>(reduce_stop - reduce_start)
                    .count();
            print_phase_result(target_name, "reduce_checksum",
                               reduce_elapsed, sorted.size(),
                               collect_phase_stats() - reduce_stats_before);

            std::cout << target_name << "_work_phase_end phase=reduce_checksum\n";
            starfish::profile::end_work();

            std::cout << target_name << "_result"
                      << " file=" << argv[2]
                      << " bytes=" << size
                      << " threads=" << chunks.size()
                      << " total_words=" << total_words
                      << " unique_words=" << sorted.size()
                      << " hashmap_size=" << global_map.size.load()
                      << " checksum=0x" << std::hex << checksum << std::dec
                      << " too_long_words=" << too_long_words
                      << " max_word_len=" << max_word_len
                      << " remote_used_bytes="
                      << starfish::allocator::remote::remote_global_heap
                             .get_used_bytes()
                      << "\n";
            for (size_t i = 0; i < std::min(top_k, top.size()); ++i) {
                std::cout << "top" << (i + 1)
                          << " word=" << top[i].first
                          << " count=" << top[i].second
                          << "\n";
            }

            starfish::runtime_quiesce_cache();
        }
        starfish::runtime_destroy();
    } catch (const std::exception &ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}

}  // namespace starfish::wordcount
