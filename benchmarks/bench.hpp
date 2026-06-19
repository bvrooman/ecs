// Minimal microbenchmark harness.
//
// bench(name, entities, body) times `body()` (one sweep over the data), running
// it `iters` times per sample and keeping the best of `repeats` samples -- best
// is the most stable estimator for an in-memory kernel (it discards scheduler
// and frequency-scaling noise). Results are reported as ns per entity per sweep.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>

namespace bench {

using clock_type = std::chrono::steady_clock;

// Defeat dead-code elimination: force `v` to be treated as observed/clobbered.
template <class T>
inline void keep(const T& v) {
  asm volatile("" : : "r,m"(v) : "memory");
}

inline int g_iters = 50;
inline int g_repeats = 5;

template <class Body>
void run(const char* name, std::size_t entities, Body&& body) {
  double best_ms = 1e300;
  for (int r = 0; r < g_repeats; ++r) {
    const auto t0 = clock_type::now();
    for (int it = 0; it < g_iters; ++it) body();
    const auto t1 = clock_type::now();
    best_ms = std::min(
        best_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  const double ns_per = best_ms * 1e6 / (g_iters * static_cast<double>(entities));
  std::printf("  %-46s %8.2f ns/entity   (best %.1f ms / %d sweeps)\n", name,
              ns_per, best_ms, g_iters);
}

} // namespace bench
