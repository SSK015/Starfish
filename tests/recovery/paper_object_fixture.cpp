#include "ft/recompute/object_recipe.hpp"
#include "lib/control.hpp"
#include "mm/cache/accessor.hpp"
#include "mm/cache/base/scope.hpp"
#include "transport/rdma/config.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
using starfish::recompute::RemoteObject;
constexpr int kInputB = 19;
constexpr int kInputC = 23;
constexpr int kExpectedA = 42;
constexpr int kMutatedA = 99;
constexpr size_t kPayloadBytes = 2 * 1024;
constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kPaddingBytes = kPayloadBytes - sizeof(int);
std::atomic<size_t> recompute_calls{0};

struct Cell {
  int value = 0;
  std::array<uint8_t, kPaddingBytes> padding{};
};
static_assert(sizeof(Cell) == kPayloadBytes);
using PressurePayload = Cell;

void recompute_a(Cell &out, const Cell &b, const Cell &c) {
  recompute_calls.fetch_add(1, std::memory_order_relaxed);
  out.value = b.value + c.value;
}

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

const char *required_env(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    throw std::runtime_error(std::string("set ") + name +
                             " to a fresh temporary marker path");
  }
  return value;
}

bool marker_exists(const std::string &path) {
  return access(path.c_str(), F_OK) == 0;
}

void write_marker(const std::string &path, const std::string &contents) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
  if (fd < 0) throw std::runtime_error("cannot create READY marker " + path);
  size_t written_total = 0;
  while (written_total < contents.size()) {
    const ssize_t written =
        write(fd, contents.data() + written_total,
              contents.size() - written_total);
    if (written <= 0) {
      close(fd);
      throw std::runtime_error("cannot write READY marker " + path);
    }
    written_total += static_cast<size_t>(written);
  }
  if (close(fd) != 0) {
    throw std::runtime_error("cannot close READY marker " + path);
  }
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

template <class T>
bool is_remote(const RemoteObject<T> &object) {
  return object.is_remote() && !object.remote_invalid();
}

template <class T>
void wait_until_remote(const RemoteObject<T> &object, const char *name) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(90);
  while (std::chrono::steady_clock::now() < deadline) {
    if (is_remote(object)) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  throw std::runtime_error(std::string(name) +
                           " did not reach an actual remote backing");
}

size_t endpoint_of(const starfish::rdma::Configure &config, uint64_t address) {
  return config.map_remote_addr(address).first;
}

}  // namespace

int main(int argc, char **argv) {
  std::cout.setf(std::ios::unitbuf);
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <seven-endpoint-config>\n";
    return 2;
  }

  try {
    starfish::rdma::Configure config;
    config.from_file(argv[1]);
    require(config.is_ec_batch_mode() && config.exclusive_cache &&
                config.ft_ec_data_shards == 4 &&
                config.ft_ec_parity_shards == 2 && config.server_count == 7 &&
                config.ft_standby_endpoint == 6,
            "requires exclusive 4+2 EC-batch mode, six active endpoints, "
            "and one standby at index 6");
    require(config.client_buffer_size >= 4 * kMiB &&
                config.client_buffer_size <= 128 * kMiB,
            "use a small RDMA fixture config with client_buffer_size "
            "between 4 and 128 MiB");

    const std::string ready_path =
        required_env("FARLIB_WC_RECIPE_READY_PATH");
    const std::string go_path = required_env("FARLIB_WC_RECIPE_GO_PATH");
    require(ready_path != go_path,
            "READY and GO marker paths must be distinct");
    require(!marker_exists(ready_path) && !marker_exists(go_path),
            "READY/GO path already exists; choose a fresh temporary directory");

    starfish::runtime_init(config);
    RuntimeGuard runtime_guard;
    {
      RemoteObject<Cell> a;
      RemoteObject<Cell> b;
      RemoteObject<Cell> c;
      {
        starfish::RootDereferenceScope s;
        (void)a.allocate_lite(s, Cell{});
      }
      {
        starfish::RootDereferenceScope s;
        (void)b.allocate_lite(s, Cell{kInputB});
      }
      {
        starfish::RootDereferenceScope s;
        (void)c.allocate_lite(s, Cell{kInputC});
      }
      {
        // One short pin chain reads b/c and writes a; no accessor survives an
        // unrelated allocation or a later pinning epoch.
        starfish::RootDereferenceScope s;
        auto b_value = b.access(s);
        auto c_value = c.access(s);
        auto a_value = a.access<true>(s);
        require(b_value.valid() && c_value.valid() && a_value.valid(),
                "initial computation could not access a, b, and c");
        recompute_a(*a_value, *b_value, *c_value);
      }
      require(recompute_calls.load(std::memory_order_relaxed) == 1,
              "normal application computation did not run exactly once");

      bool mark_with_live_writer_rejected = false;
      {
        starfish::RootDereferenceScope s;
        auto b_writer = b.access<true>(s);
        require(b_writer.valid(), "could not acquire initial writer for b");
        mark_with_live_writer_rejected =
            !s.mark_recomputable(a, {b, c}, recompute_a, {a, b, c});
        require(!a.has_recompute_recipe(),
                "failed registration left a recipe bound to a");
      }
      require(mark_with_live_writer_rejected,
              "mark_recomputable accepted input b with a live writer");

      {
        starfish::RootDereferenceScope s;
        require(s.mark_recomputable(a, {b, c}, recompute_a, {a, b, c}),
                "paper-style mark_recomputable registration failed");
      }
      require(a.has_recompute_recipe(),
              "successful registration did not bind a's recipe");

      bool b_write_blocked = false;
      bool c_write_blocked = false;
      {
        starfish::RootDereferenceScope s;
        auto b_writer = b.access<true>(s);
        b_write_blocked = !b_writer.valid();
        require(a.has_recompute_recipe(),
                "attempted b write detached a's recipe");
        auto c_writer = c.access<true>(s);
        c_write_blocked = !c_writer.valid();
        require(a.has_recompute_recipe(),
                "attempted c write detached a's recipe");
      }
      require(b_write_blocked && c_write_blocked,
              "a live recipe did not reject typed writes to b and c");

      const size_t pressure_count =
          3 * ((config.client_buffer_size + kPayloadBytes - 1) /
               kPayloadBytes) +
          512;
      std::vector<RemoteObject<PressurePayload>> pressure;
      pressure.reserve(pressure_count);
      for (size_t i = 0; i < pressure_count; ++i) {
        pressure.emplace_back();
        starfish::RootDereferenceScope s;
        auto payload = pressure.back().allocate_lite(s);
        (*payload).value = static_cast<int>(i);
        std::memset((*payload).padding.data(), static_cast<int>(i & 0xff),
                    (*payload).padding.size());
      }

      wait_until_remote(a, "a");
      wait_until_remote(b, "b");
      wait_until_remote(c, "c");
      require(a.has_recompute_recipe(),
              "a lost its recipe before failure injection");
      require(recompute_calls.load(std::memory_order_relaxed) == 1,
              "recipe ran before the endpoint failure");

      const size_t target_endpoint = endpoint_of(config, a.remote_addr());
      const size_t b_endpoint = endpoint_of(config, b.remote_addr());
      const size_t c_endpoint = endpoint_of(config, c.remote_addr());
      require(target_endpoint < 6 && b_endpoint < 6 && c_endpoint < 6,
              "a, b, and c must have remote backing on active endpoints");
      const auto [target_addr, target_port] = config.get_endpoint(target_endpoint);
      const auto [b_addr, b_port] = config.get_endpoint(b_endpoint);
      const auto [c_addr, c_port] = config.get_endpoint(c_endpoint);

      const std::string ready_line =
          "paper_object_recovery_ready"
          " target_endpoint=" + std::to_string(target_endpoint) +
          " target_addr=" + target_addr +
          " target_port=" + target_port +
          " a_remote=true b_remote=true c_remote=true"
          " b_endpoint=" + std::to_string(b_endpoint) +
          " b_addr=" + b_addr + " b_port=" + b_port +
          " c_endpoint=" + std::to_string(c_endpoint) +
          " c_addr=" + c_addr + " c_port=" + c_port +
          " ec=4+2 standby=6 pressure_bytes=" +
          std::to_string(pressure_count * kPayloadBytes);
      write_marker(ready_path, ready_line + "\n");
      std::cout << ready_line << "\n";
      wait_for_go_marker(go_path);

      const auto before_fault =
          starfish::Cache::get_default()->recomputable_stats();
      int restored_a = 0;
      {
        starfish::RootDereferenceScope s;
        auto restored = a.access(s);
        require(restored.valid(),
                "automatic recipe recovery returned an invalid object");
        restored_a = restored->value;
      }
      require(restored_a == kExpectedA,
              "recomputed a differs from normal application output");
      require(recompute_calls.load(std::memory_order_relaxed) == 2,
              "the common computation callback did not run exactly once "
              "during recovery");
      require(a.has_recompute_recipe(),
              "recipe was unexpectedly lost before the mutation check");
      const auto after_restore =
          starfish::Cache::get_default()->recomputable_stats();
      require(after_restore.restored == before_fault.restored + 1,
              "runtime did not record exactly one recipe recovery");
      require(after_restore.failures == before_fault.failures,
              "runtime reported a failed recipe recovery");

      {
        starfish::RootDereferenceScope s;
        auto mutated = a.access<true>(s);
        require(mutated.valid(),
                "could not acquire mutable access to restored a");
        mutated->value = kMutatedA;
      }
      require(!a.has_recompute_recipe(),
              "mutation left the old recipe attached");

      size_t pressure_reads = 0;
      size_t checksum = 0;
      for (size_t sweep = 0; sweep < 4 && !is_remote(a); ++sweep) {
        for (const auto &object : pressure) {
          starfish::RootDereferenceScope s;
          auto payload = object.access(s);
          require(payload.valid(), "pressure object read failed");
          checksum += (*payload).padding[sweep % kPaddingBytes];
          ++pressure_reads;
        }
      }
      require(is_remote(a),
              "cache pressure did not evict mutated a to remote EC storage");

      int readback = 0;
      {
        starfish::RootDereferenceScope s;
        auto restored = a.access(s);
        require(restored.valid(),
                "ordinary EC readback after recipe invalidation failed");
        readback = restored->value;
      }
      require(readback == kMutatedA,
              "EC readback did not preserve the mutated a value");
      require(!a.has_recompute_recipe(),
              "old recipe reappeared after ordinary remote readback");
      require(recompute_calls.load(std::memory_order_relaxed) == 2,
              "old recipe callback ran again after mutation and re-eviction");
      const auto after_test =
          starfish::Cache::get_default()->recomputable_stats();
      require(after_test.restored == after_restore.restored,
              "ordinary readback unexpectedly reran recipe recovery");
      require(after_test.failures == before_fault.failures,
              "runtime reported a recovery failure during the fixture");

      bool b_write_released = false;
      {
        starfish::RootDereferenceScope s;
        auto writable_b = b.access<true>(s);
        b_write_released = writable_b.valid();
        require(b_write_released,
                "b remained write-protected after a's recipe was detached");
        writable_b->value = kInputB + 1;
      }
      {
        starfish::RootDereferenceScope s;
        auto read_b = b.access(s);
        require(read_b.valid() && read_b->value == kInputB + 1,
                "b did not retain its post-detachment write");
      }
      require(!a.has_recompute_recipe(),
              "b write unexpectedly restored a's stale recipe");
      require(recompute_calls.load(std::memory_order_relaxed) == 2,
              "old callback ran during the post-detachment b write");

      std::cout << "paper_object_recovery_result status=pass"
                << " recipe_restored="
                << after_test.restored - before_fault.restored
                << " callback_calls="
                << recompute_calls.load(std::memory_order_relaxed)
                << " mutated_value=" << readback
                << " pressure_reads=" << pressure_reads
                << " checksum=" << checksum
                << " old_recipe_replays_after_mutation=0"
                << " mark_with_live_writer_rejected="
                << (mark_with_live_writer_rejected ? "true" : "false")
                << " b_write_blocked=" << (b_write_blocked ? "true" : "false")
                << " c_write_blocked=" << (c_write_blocked ? "true" : "false")
                << " b_write_released=" << (b_write_released ? "true" : "false")
                << " runtime_failures="
                << after_test.failures - before_fault.failures << "\n";
    }
    runtime_guard.shutdown();
  } catch (const std::exception &ex) {
    std::cerr << "paper_object_recovery_result status=fail reason="
              << ex.what() << "\n";
    return 1;
  }
  return 0;
}
