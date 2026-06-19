// Minimal test harness: CHECK(expr) records a failure but keeps going; main
// returns non-zero if anything failed. Wrap comma-containing expressions in
// extra parens, e.g. CHECK((query<A, B>(w).count() == 2)).
#pragma once
#include <cstdio>

namespace ecstest {
inline int g_failures = 0;
inline int g_checks = 0;
} // namespace ecstest

#define CHECK(expr)                                                            \
  do {                                                                         \
    ++::ecstest::g_checks;                                                     \
    if (!(expr)) {                                                             \
      ++::ecstest::g_failures;                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                          \
  } while (0)

#define RUN_SUITE(fn)                                                          \
  do {                                                                         \
    std::printf("[ run ] %s\n", #fn);                                          \
    fn();                                                                      \
  } while (0)

#define REPORT()                                                              \
  (std::printf("%d/%d checks passed\n",                                        \
               ::ecstest::g_checks - ::ecstest::g_failures,                    \
               ::ecstest::g_checks),                                           \
   ::ecstest::g_failures == 0 ? 0 : 1)
