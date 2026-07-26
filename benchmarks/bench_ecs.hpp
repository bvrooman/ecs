// ECS-aware benchmark helpers.
//
// bench_system() times a real system's steady-state run over a world -- the
// production execution path -- reported as ns/entity. A mutating kernel is
// measured exactly as the scheduler runs it: ad-hoc World iteration is read-only
// by design, so a write goes through a system's declared-write Query, and that
// is what we benchmark. The system is registered once (outside the timed loop);
// only schedule.run() is timed. A single serial system flattens to one work item
// spanning every matching archetype, so the per-run scheduler overhead is O(1)
// against an O(entities) kernel -- negligible at benchmark sizes, so layout and
// access-shape signals survive.
//
// Reads need no system: call World's read-only for_each / for_each_chunk / count
// directly.
#pragma once

#include "bench.hpp"
#include <ecs/ecs.hpp>

template <class Sys>
void bench_system(char const* name, std::size_t n, ecs::World& world, Sys sys) {
    ecs::Schedule s;
    s.add(name, std::move(sys));
    bench::run(name, n, [&] { s.run(world); });
}

// The min_ns_per (best-of-repeats) cousin, returning ns/entity for a system's
// steady-state run -- for suites that emit their own report rows (e.g. gather).
template <class Sys>
double ns_per_system(std::size_t n, int repeats, ecs::World& world, Sys sys) {
    ecs::Schedule s;
    s.add("bench", std::move(sys));
    return bench::min_ns_per(n, repeats, [&] { s.run(world); });
}
