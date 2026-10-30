#pragma once

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <iostream>

namespace starfish {

namespace debug {

inline void assert_fail(const char *expr, const char *file, int line,
                        const char *function) {
  std::cerr << "Assertion failed: " << expr << " at " << file << ":" << line
            << " in function " << function << std::endl;
  abort();
}

inline void check_err(int err, const char *msg) {
  if (err != 0) {
    std::cerr << "Error (" << errno << ") on " << msg << std::endl;
    abort();
  }
}

[[noreturn]] inline void todo(const char *msg) {
  std::cerr << "TODO: " << msg << std::endl;
  abort();
}

[[noreturn]] inline void error(const char *msg) {
  std::cerr << "ERROR: " << msg << std::endl;
  abort();
}

inline void warn(const char *msg) { std::cerr << "WARN: " << msg << std::endl; }

inline bool verbose_log_enabled() {
  static const bool enabled = [] {
    const char *value = std::getenv("FARLIB_VERBOSE_LOG");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

inline void info(const char *msg) {
  if (verbose_log_enabled()) {
    std::fprintf(stdout, "INFO: %s\n", msg);
  }
}

}  // namespace debug

}  // namespace starfish

#define TODO(MSG) starfish::debug::todo(MSG)
#define ERROR(MSG) starfish::debug::error(MSG)
#define WARN(MSG) starfish::debug::warn(MSG)
#define INFO(MSG) starfish::debug::info(MSG)
#define CHECK_ERR(X) starfish::debug::check_err(X, #X)
#define DEBUG_ASSERT assert
#define ASSERT(X)         \
    (static_cast<bool>(X) \
         ? void(0)        \
         : starfish::debug::assert_fail(#X, __FILE__, __LINE__, __func__))
