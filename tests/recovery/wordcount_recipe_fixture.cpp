#include "wordcount_common.hpp"
#include "data_structure/partitioned_hashmaps.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace {

using namespace starfish::wordcount;
using NativeCountMap = FarWordMap::DefaultRecipeTable;

struct RestoreEvent {
    uint64_t ordinal{0};
    FixedWord word{};
    CountValue value{};
    size_t successes{0};
};

class RestoreProbe {
 public:
    void arm() {
        std::lock_guard<std::mutex> lock(mu_);
        armed_ = true;
    }

    void record(const FixedWord &word, const CountValue &value,
                uint64_t ordinal) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!armed_) return;
        for (auto &event : events_) {
            if (event.ordinal == ordinal) {
                ++event.successes;
                return;
            }
        }
        events_.push_back(RestoreEvent{ordinal, word, value, 1});
    }

    std::optional<RestoreEvent> first_event() const {
        std::lock_guard<std::mutex> lock(mu_);
        if (events_.empty()) return std::nullopt;
        return events_.front();
    }

    size_t success_count(uint64_t ordinal) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto &event : events_) {
            if (event.ordinal == ordinal) return event.successes;
        }
        return 0;
    }

    size_t success_total() const {
        std::lock_guard<std::mutex> lock(mu_);
        size_t total = 0;
        for (const auto &event : events_) total += event.successes;
        return total;
    }

 private:
    mutable std::mutex mu_;
    bool armed_{false};
    std::vector<RestoreEvent> events_;
};

// Keep fixture observations in the replay table, so publication exercises the
// same scope annotation and default key/value projector as the application.
class FixtureCountMap : public NativeCountMap {
 public:
    class ReplayTable : public NativeCountMap::ReplayTable {
     public:
        std::shared_ptr<RestoreProbe> restore_probe;

        template <class Fn>
        bool visit_ordinal(uint64_t ordinal, Fn &&fn) const {
            return NativeCountMap::ReplayTable::visit_ordinal(
                ordinal, [&](const FixedWord &word, const CountValue &value,
                             uint64_t id) {
                    if (!std::invoke(fn, word, value, id)) return false;
                    restore_probe->record(word, value, id);
                    return true;
                });
        }
    };
};

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

void write_marker(const std::string &path, const std::string &contents) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("cannot create ready marker " + path);
    }
    size_t written_total = 0;
    while (written_total < contents.size()) {
        const ssize_t written =
            write(fd, contents.data() + written_total,
                  contents.size() - written_total);
        if (written <= 0) {
            close(fd);
            throw std::runtime_error("cannot write ready marker " + path);
        }
        written_total += static_cast<size_t>(written);
    }
    if (close(fd) != 0) {
        throw std::runtime_error("cannot close ready marker " + path);
    }
}

bool marker_exists(const std::string &path) {
    return access(path.c_str(), F_OK) == 0;
}

void wait_for_go_marker(const std::string &path) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::minutes(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (marker_exists(path)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("timed out waiting for GO marker " + path);
}

size_t scan_pressure_map(FarWordMap &map) {
    size_t scanned = 0;
    starfish::RootDereferenceScope scope;
    map.for_each_locked(
        [&](const FixedWord &, const CountValue &, auto &) { ++scanned; },
        scope);
    return scanned;
}

class RuntimeGuard {
 public:
    ~RuntimeGuard() {
        if (active_) {
            starfish::runtime_quiesce_cache();
            starfish::runtime_destroy();
        }
    }

    void shutdown() {
        if (!active_) return;
        starfish::runtime_quiesce_cache();
        starfish::runtime_destroy();
        active_ = false;
    }

 private:
    bool active_{true};
};

const char *required_env(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        throw std::runtime_error(std::string("set ") + name +
                                 " to a fresh temporary marker path");
    }
    return value;
}

}  // namespace

int main(int argc, char **argv) {
    std::cout.setf(std::ios::unitbuf);
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <config> <fixture-input>\n";
        return 2;
    }

    try {
        starfish::rdma::Configure config;
        config.from_file(argv[1]);
        require(config.is_ec_batch_mode() && config.exclusive_cache &&
                    config.ft_ec_data_shards == 4 &&
                    config.ft_ec_parity_shards == 2 &&
                    config.ft_standby_endpoint >= 0 &&
                    config.server_count >= 7,
                "fixture recovery needs 4+2 EC-batch, exclusive cache, six "
                "active endpoints, and one configured standby");

        const std::string ready_path =
            required_env("FARLIB_WC_RECIPE_READY_PATH");
        const std::string go_path = required_env("FARLIB_WC_RECIPE_GO_PATH");
        require(ready_path != go_path,
                "READY and GO markers must use distinct paths");
        require(!marker_exists(ready_path),
                "READY marker already exists; choose a fresh temporary directory");
        require(!marker_exists(go_path),
                "GO marker already exists; choose a fresh temporary directory");

        auto file = std::make_shared<MappedFile>(argv[2]);
        require(file->size != 0, "fixture input must not be empty");

        const Chunk input{0, file->size};
        auto pressure_probe = std::make_shared<RestoreProbe>();
        auto count_partition = [file, pressure_probe](auto &rows,
                                                     const Chunk &chunk) {
            if constexpr (std::remove_reference_t<decltype(rows)>::replay_build) {
                rows.restore_probe = pressure_probe;
            }
            return count_chunk_native(rows, file->data, chunk);
        };
        auto counts = std::make_unique<FixtureCountMap>();
        counts->reserve(1u << 20);
        ThreadStats initial_stats =
            count_chunk_native(*counts, file->data, input);
        require(counts->size() == 65536,
                "expected the deterministic 65,536-key Wordcount fixture");

        const uint64_t total_words = initial_stats.total_words;

        starfish::runtime_init(config);
        RuntimeGuard runtime_guard;
        {
            constexpr uint32_t kFixtureMapShift = 17;
            starfish::PartitionedHashMaps<FarWordMap> local_maps(
                1, kFixtureMapShift);
            auto recipe_map = local_maps[0];

            {
                starfish::RootDereferenceScope scope;
                require(scope.mark_recomputable<FixtureCountMap>(
                            local_maps[0], input, count_partition),
                        "failed to mark recipe-backed fixture partition");
                require(local_maps.publish(0, *counts, scope),
                        "failed to populate recipe-backed fixture map");
            }

            // A separate ordinary map drives cache pressure and receives EC
            // protection on every eviction.
            FarWordMap pressure_map(kFixtureMapShift);
            {
                starfish::RootDereferenceScope scope;
                require(FarWordMap::publish_ordinary(
                            pressure_map, *counts, scope),
                        "failed to populate EC-protected pressure map");
            }

            const std::string ready_line =
                "wordcount_recipe_fixture_ready unique_words=" +
                std::to_string(counts->size()) +
                " total_words=" + std::to_string(total_words) +
                " endpoints=" + std::to_string(config.server_count) +
                " active=6 standby=1";
            require(!marker_exists(go_path),
                    "GO marker appeared before READY; choose a fresh directory");
            write_marker(ready_path, ready_line + "\n");
            std::cout << ready_line << "\n";
            wait_for_go_marker(go_path);
            pressure_probe->arm();


            const auto before_fault =
                starfish::Cache::get_default()->recomputable_stats();
            std::optional<RestoreEvent> restored_target;
            const bool completed_scan = counts->visit(
                [&](const FixedWord &word, const CountValue &expected,
                    uint64_t ordinal) {
                    CountValue value{};
                    bool found = false;
                    const auto before_lookup =
                        starfish::Cache::get_default()->recomputable_stats();
                    {
                        starfish::RootDereferenceScope scope;
                        found = recipe_map->get(word, &value, scope);
                    }
                    if (!found) {
                        const auto stats =
                            starfish::Cache::get_default()->recomputable_stats();
                        const auto callback_target =
                            pressure_probe->first_event();
                        std::cerr << "wordcount_recipe_fixture_diagnostic"
                                  << " failure=lookup"
                                  << " missing_word=" << word.view()
                                  << " missing_ordinal=" << ordinal
                                  << " expected_count=" << expected.count
                                  << " map_size=" << recipe_map->size.load()
                                  << " registered=" << stats.registered
                                  << " restored=" << stats.restored
                                  << " failures=" << stats.failures
                                  << " callback_successes="
                                  << pressure_probe->success_total();
                        if (callback_target.has_value()) {
                            std::cerr << " callback_target_word="
                                      << callback_target->word.view()
                                      << " callback_target_ordinal="
                                      << callback_target->ordinal
                                      << " callback_target_successes="
                                      << pressure_probe->success_count(
                                             callback_target->ordinal);
                        } else {
                            std::cerr << " callback_target=none";
                        }
                        std::cerr << "\n";
                        throw std::runtime_error(
                            "fixture map lookup failed after node loss");
                    }
                    require(value.count == expected.count,
                            "fixture count changed during initial recovery");
                    const auto after_lookup =
                        starfish::Cache::get_default()->recomputable_stats();
                    if (after_lookup.restored > before_lookup.restored) {
                        restored_target = pressure_probe->first_event();
                        require(restored_target.has_value(),
                                "restored output was not observed by the default projector");

                        bool target_matches_native = false;
                        counts->visit(
                            [&](const FixedWord &native_word,
                                const CountValue &native_value,
                                uint64_t native_ordinal) {
                                if (native_ordinal != restored_target->ordinal)
                                    return true;
                                target_matches_native =
                                    native_word == restored_target->word &&
                                    native_value.count ==
                                        restored_target->value.count;
                                return false;
                            });
                        require(target_matches_native,
                                "default ordinal projector restored a non-native row");

                        CountValue restored_target_value{};
                        {
                            starfish::RootDereferenceScope scope;
                            require(recipe_map->get(
                                        restored_target->word,
                                        &restored_target_value, scope),
                                    "projected callback key is absent from the map");
                        }
                        require(restored_target_value.count ==
                                    restored_target->value.count,
                                "projected callback value differs from the map");
                        return false;
                    }
                    return true;
                });
            require(!completed_scan && restored_target.has_value(),
                    "no Wordcount recipe callback restored an output");
            const RestoreEvent restore = *restored_target;
            const CountValue expected = restore.value;
            const size_t callbacks_before_mutation =
                pressure_probe->success_count(restore.ordinal);
            require(callbacks_before_mutation == 1,
                    "target ordinal did not have exactly one successful projection");

            CountValue restored_value{};
            {
                starfish::RootDereferenceScope scope;
                require(recipe_map->get(restore.word, &restored_value, scope),
                        "captured restored key is absent from the map");
            }
            require(restored_value.count == expected.count,
                    "captured restored value differs from the native count");

            const uint64_t sentinel = expected.count + 1;
            {
                starfish::RootDereferenceScope scope;
                require(recipe_map->put(
                            restore.word,
                            make_count_value(restore.word, sentinel), scope),
                        "ordinary put failed for the restored key");
            }
            const size_t callbacks_after_put =
                pressure_probe->success_count(restore.ordinal);
            require(callbacks_after_put == callbacks_before_mutation,
                    "ordinary put unexpectedly reran the target callback");

            size_t pressure_objects = 0;
            bool re_evicted = false;
            bool sentinel_preserved = false;
            size_t target_callbacks_after_read = callbacks_after_put;
            for (size_t sweep = 0; sweep < 4; ++sweep) {
                pressure_objects += scan_pressure_map(pressure_map);
                CountValue value{};
                bool was_remote = false;
                bool found = false;
                {
                    starfish::RootDereferenceScope scope;
                    found = recipe_map->get_with_remote_status(
                        restore.word, &value, was_remote, scope);
                }
                require(found, "mutated key failed to read after pressure");
                require(value.count == sentinel,
                        "mutated value was lost after cache pressure");
                target_callbacks_after_read =
                    pressure_probe->success_count(restore.ordinal);
                require(target_callbacks_after_read ==
                            callbacks_before_mutation,
                        "re-evicted target incorrectly used its old recipe");
                if (was_remote) {
                    re_evicted = true;
                    sentinel_preserved = true;
                    break;
                }
            }

            const auto after_test =
                starfish::Cache::get_default()->recomputable_stats();
            require(re_evicted,
                    "target never became remote after cache pressure");
            require(sentinel_preserved,
                    "mutated value was not preserved by EC readback");
            require(after_test.restored > before_fault.restored,
                    "runtime did not report a successful recipe restore");
            require(after_test.failures == 0,
                    "runtime reported a failed recipe recovery");

            std::cout << "wordcount_recipe_fixture_result"
                      << " status=pass"
                      << " recipe_restored="
                      << after_test.restored - before_fault.restored
                      << " runtime_failures=" << after_test.failures
                      << " target_ordinal=" << restore.ordinal
                      << " target_callbacks=" << callbacks_before_mutation
                      << " sentinel=" << sentinel
                      << " pressure_objects=" << pressure_objects
                      << " re_evict=observed"
                      << " sentinel_preserved=true"
                      << " target_callback_after_mutation="
                      << target_callbacks_after_read << "\n";
        }
        runtime_guard.shutdown();
    } catch (const std::exception &ex) {
        std::cerr << "wordcount_recipe_fixture_result status=fail reason="
                  << ex.what() << "\n";
        return 1;
    }
    return 0;
}
