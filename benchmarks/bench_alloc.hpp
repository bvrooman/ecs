// Global allocation counter for tick-loop suites: include this from exactly
// ONE translation unit per executable (it defines the global operator new /
// delete). allocs_per_tick is the noise-free metric -- a regression there is
// always real, unlike wall time.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace bench {
inline std::atomic<long> g_allocs {0};

// Allocations counted while `fn()` runs, divided by `per`.
template <class Fn>
double count_allocs(double const per, Fn&& fn) {
    long const a0 = g_allocs.load(std::memory_order_relaxed);
    fn();
    return double(g_allocs.load(std::memory_order_relaxed) - a0) / per;
}
} // namespace bench

void* operator new(std::size_t n) {
    bench::g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
