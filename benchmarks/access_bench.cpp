// benchmarks/access_bench.cpp
//
// Random-access and query-matching costs -- the paths the iteration suites
// skip entirely:
//
//   * get sequential / get random -- world.get<C>(entity) by handle, in
//     serial-walk order vs a shuffled order (generation check + archetype
//     indirection; the random case adds the cache miss per lookup)
//   * alive                       -- the bare generation check
//   * query over 1 vs 64 archetypes -- same total rows, fragmented into 64
//     archetypes via tag combinations: per-entity cost of archetype matching
//     + chunk boundaries (the edge-cache/matching overhead)
//
//   ECS_ENTITIES (200000), ECS_REPEATS (25)

#include "bench.hpp"
#include "ecs/ecs.hpp"
#include <algorithm>
#include <cstdio>
#include <random>
#include <span>
#include <vector>

using namespace ecs;

struct P {
    float x, y, z;
};

// Six independent tag bits -> up to 64 distinct archetypes, all carrying P.
template <int>
struct Tag {
    std::uint8_t _;
};

namespace {

void run_setup(World& w, auto fn) {
    Schedule s;
    s.add_once("setup", std::move(fn));
    s.run(w);
}

std::vector<Entity> live_entities(World& w) {
    std::vector<Entity> es;
    es.reserve(w.size());
    w.for_each_chunk<P const>([&](std::span<Entity> ids, chunk<P const>) {
        es.insert(es.end(), ids.begin(), ids.end());
    });
    return es;
}

// Spawn with the tag combination selected by the low 6 bits of `bits`.
void spawn_tagged(Commands& cmd, std::size_t i, unsigned bits) {
    P const p {float(i), 0, 0};
    auto with = [&]<int... Bs>(std::integer_sequence<int, Bs...>) {
        // Dispatch over the 64 combinations by chaining adds after spawn.
        Entity const e = cmd.spawn(p);
        ((bits >> Bs & 1u ? cmd.add(e, Tag<Bs> {}) : void()), ...);
    };
    with(std::make_integer_sequence<int, 6> {});
}

void report(char const* name, std::size_t n, int repeats, double ns) {
    std::printf("  %-26s %8.2f ns\n", name, ns);
    bench::emit(name, "ns_per_op", ns, "ns", "lower", n, 1,
                std::size_t(repeats));
}

} // namespace

int main() {
    bench::set_suite("access");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 200'000);
    int const R         = int(bench::env_long("ECS_REPEATS", 25));

    std::printf("access_bench: %zu entities, best of %d\n", N, R);

    // one flat archetype for the handle-access cases
    World w;
    run_setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < N; ++i)
            cmd.spawn(P {float(i), 0, 0});
    });
    auto es = live_entities(w);

    {
        float s = 0;
        double const ns = bench::min_ns_per(N, R, [&] {
            for (Entity const e : es)
                s += w.get<P>(e).x;
            bench::keep(s);
        });
        report("get<P> sequential", N, R, ns);
    }
    {
        std::mt19937 rng {7u};
        std::shuffle(es.begin(), es.end(), rng);
        float s = 0;
        double const ns = bench::min_ns_per(N, R, [&] {
            for (Entity const e : es)
                s += w.get<P>(e).x;
            bench::keep(s);
        });
        report("get<P> random order", N, R, ns);
    }
    {
        unsigned long alive = 0;
        double const ns = bench::min_ns_per(N, R, [&] {
            for (Entity const e : es)
                alive += w.alive(e);
            bench::keep(alive);
        });
        report("alive check", N, R, ns);
    }

    // query iteration: 1 archetype vs the same rows spread over 64
    {
        float s1 = 0;
        double const one = bench::min_ns_per(N, R, [&] {
            w.for_each_chunk<P const>(
                [&](std::span<Entity>, chunk<P const> p) {
                    for (float x : p.column<0>())
                        s1 += x;
                });
            bench::keep(s1);
        });
        report("query, 1 archetype", N, R, one);

        World frag;
        run_setup(frag, [&](Commands& cmd) {
            for (std::size_t i = 0; i < N; ++i)
                spawn_tagged(cmd, i, unsigned(i) & 63u);
        });
        float s64 = 0;
        double const many = bench::min_ns_per(N, R, [&] {
            frag.for_each_chunk<P const>(
                [&](std::span<Entity>, chunk<P const> p) {
                    for (float x : p.column<0>())
                        s64 += x;
                });
            bench::keep(s64);
        });
        report("query, 64 archetypes", N, R, many);
        bench::emit("fragmentation_overhead", "ratio", many / one, "x", "lower",
                    N, 1, std::size_t(R));
        std::printf("  %-26s %8.2fx\n", "fragmentation overhead", many / one);
    }
    return 0;
}
