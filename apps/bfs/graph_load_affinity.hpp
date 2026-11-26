#pragma once

#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <cstddef>
#include <iostream>
#include <string>
#include <system_error>

// Capture before runtime_init pins libfibre workers. Only native graph-loader
// threads apply this launch mask; fibre workers are never widened.
class GraphLoadAffinity {
public:
    GraphLoadAffinity() {
        CPU_ZERO(&cpus_);
        if (sched_getaffinity(0, sizeof(cpus_), &cpus_) != 0)
            throw std::system_error(errno, std::generic_category(),
                                    "capture graph-loader launch affinity");
    }

    int apply_to_current_thread() const {
        const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus_),
                                              &cpus_);
        if (rc != 0) return rc;
        cpu_set_t effective;
        CPU_ZERO(&effective);
        if (sched_getaffinity(0, sizeof(effective), &effective) != 0) return errno;
        return CPU_EQUAL(&effective, &cpus_) ? 0 : EINVAL;
    }

    void apply(const char *phase, std::size_t, bool first_for_thread) const {
        if (!first_for_thread) return;
        const char *name = std::string(phase) == "read" ? "graph-read" : "graph-fill";
        (void)pthread_setname_np(pthread_self(), name);
        const int rc = apply_to_current_thread();
        if (rc != 0)
            throw std::system_error(rc, std::generic_category(),
                                    "apply graph-loader affinity");
    }

    void report_phase(const char *phase, std::size_t threads,
                      std::size_t applied, int error) const {
        std::cout << "graph_load_affinity phase=" << phase
                  << " threads=" << threads << " applied=" << applied
                  << " cpu_count=" << CPU_COUNT(&cpus_)
                  << " cpus=" << cpu_list()
                  << " error=" << error << std::endl;
        if (error != 0)
            std::cerr << "Graph loader affinity failed: "
                      << std::system_category().message(error) << std::endl;
    }

    int cpu_count() const { return CPU_COUNT(&cpus_); }
    std::string cpu_list() const {
        std::string result;
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
            if (!CPU_ISSET(cpu, &cpus_)) continue;
            if (!result.empty()) result += ',';
            result += std::to_string(cpu);
        }
        return result;
    }

private:
    cpu_set_t cpus_{};
};
