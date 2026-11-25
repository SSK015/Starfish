#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

#include "mm/allocator/region_remote_allocator.hpp"
#include "mm/cache/cache.hpp"
#include "data_structure/concurrent_hashmap.hpp"
#include "transport/rdma/config.hpp"
#include "lib/control.hpp"
#include "lib/debug.hpp"
#include "async/parallel.hpp"
#include "async/spinlock.hpp"
#include "profiling/stats.hpp"

namespace starfish::wordcount {

constexpr size_t kMaxWordBytes = 512;
constexpr size_t kGlobalLockCount = 4096;

struct MappedFile {
    int fd{-1};
    const char *data{nullptr};
    size_t size{0};

    explicit MappedFile(const char *path) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error(std::string("open failed: ") + strerror(errno));
        }
        struct stat st {};
        if (fstat(fd, &st) != 0) {
            throw std::runtime_error(std::string("fstat failed: ") + strerror(errno));
        }
        size = static_cast<size_t>(st.st_size);
        if (size == 0) {
            return;
        }
        void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped == MAP_FAILED) {
            throw std::runtime_error(std::string("mmap failed: ") + strerror(errno));
        }
        data = static_cast<const char *>(mapped);
    }

    ~MappedFile() {
        if (data != nullptr) {
            munmap(const_cast<char *>(data), size);
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
};

struct Chunk {
    size_t begin{0};
    size_t end{0};
};

struct FixedWord {
    uint16_t len{0};
    std::array<char, kMaxWordBytes> bytes{};

    std::string_view view() const { return {bytes.data(), len}; }

    bool operator==(const FixedWord &other) const {
        return len == other.len &&
               std::memcmp(bytes.data(), other.bytes.data(), len) == 0;
    }
};

struct FixedWordHash {
    size_t operator()(const FixedWord &word) const {
        uint64_t hash = 1469598103934665603ull;
        for (uint16_t i = 0; i < word.len; ++i) {
            hash ^= static_cast<unsigned char>(word.bytes[i]);
            hash *= 1099511628211ull;
        }
        return static_cast<size_t>(hash);
    }
};

struct CountValue {
    uint64_t count{0};
};

using FarWordMap = starfish::ConcurrentHashMap<FixedWord, CountValue, FixedWordHash>;

struct ThreadStats {
    uint64_t total_words{0};
    uint64_t too_long_words{0};
    uint64_t max_word_len{0};
    uint64_t native_unique_words{0};
};

inline bool is_word_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z');
}

inline char lower_ascii(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return static_cast<char>(c);
}

inline size_t advance_to_delimiter(const char *data, size_t pos, size_t limit) {
    while (pos < limit && is_word_char(static_cast<unsigned char>(data[pos]))) {
        ++pos;
    }
    return pos;
}

inline std::vector<Chunk> make_chunks(const char *data, size_t size, size_t threads) {
    std::vector<Chunk> chunks;
    if (threads == 0) {
        return chunks;
    }
    if (size == 0) {
        chunks.push_back({0, 0});
        return chunks;
    }
    threads = std::min(threads, size);
    chunks.reserve(threads);
    size_t begin = 0;
    for (size_t i = 0; i < threads; ++i) {
        size_t end = (i + 1 == threads) ? size : ((i + 1) * size / threads);
        if (i + 1 != threads) {
            end = advance_to_delimiter(data, end, size);
        }
        chunks.push_back({begin, end});
        begin = end;
    }
    return chunks;
}

inline uint64_t fnv1a_bytes(uint64_t hash, const void *ptr, size_t len) {
    const auto *bytes = static_cast<const unsigned char *>(ptr);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline uint64_t checksum_counts(const std::vector<std::pair<std::string, uint64_t>> &items) {
    uint64_t hash = 1469598103934665603ull;
    for (const auto &item : items) {
        hash = fnv1a_bytes(hash, item.first.data(), item.first.size());
        const char sep = '\0';
        hash = fnv1a_bytes(hash, &sep, sizeof(sep));
        uint64_t count = item.second;
        hash = fnv1a_bytes(hash, &count, sizeof(count));
    }
    return hash;
}

inline void fill_payload(CountValue &value, const FixedWord &word) {
    (void)value;
    (void)word;
}

inline bool read_word(const char *data, size_t &pos, size_t end, FixedWord &word,
                      ThreadStats &stats) {
    while (pos < end && !is_word_char(static_cast<unsigned char>(data[pos]))) {
        ++pos;
    }
    if (pos >= end) {
        return false;
    }

    word = {};
    size_t len = 0;
    while (pos < end && is_word_char(static_cast<unsigned char>(data[pos]))) {
        if (len < kMaxWordBytes) {
            word.bytes[len] = lower_ascii(static_cast<unsigned char>(data[pos]));
        }
        ++len;
        ++pos;
    }
    stats.max_word_len = std::max<uint64_t>(stats.max_word_len, len);
    if (len > kMaxWordBytes) {
        ++stats.too_long_words;
        return false;
    }
    word.len = static_cast<uint16_t>(len);
    ++stats.total_words;
    return true;
}

inline CountValue make_count_value(const FixedWord &word, uint64_t count) {
    CountValue value{};
    value.count = count;
    fill_payload(value, word);
    return value;
}

inline void add_to_map(FarWordMap &map, const FixedWord &word, uint64_t delta,
                starfish::DereferenceScope &scope) {
    CountValue value{};
    uint64_t count = delta;
    if (map.get(word, &value, scope)) {
        count += value.count;
    }
    ASSERT(map.put(word, make_count_value(word, count), scope));
}

inline ThreadStats count_chunk_native(auto &counts, const char *data,
                                     Chunk chunk) {
    ThreadStats stats;
    FixedWord word;
    size_t pos = chunk.begin;
    while (pos < chunk.end) {
        if (read_word(data, pos, chunk.end, word, stats)) {
            ++counts[word].count;
        }
    }
    stats.native_unique_words = counts.size();
    return stats;
}

}  // namespace starfish::wordcount
