// benchmarks/sort_bench.cpp
//
// World::sort_rows mechanics (the spatial-locality maintenance pass):
//
//   * sorted scan   -- re-sorting an already-sorted archetype: the keys-pass
//                      early exit, the cost paid on every quiet maintenance
//   * shuffled sort -- full counting sort + permutation of a random layout,
//                      the worst-case maintenance tick
//   * drift + skip  -- 5% churn (destroy + respawn) then sort_rows with
//                      min_disorder above the drift: measures the skip path
//   * drift + sort  -- same drift, threshold 0: the real periodic re-sort
//
// Key = compact grid cell (the counting-sort path, like mist's). Reported as
// ns/row, best of R (setup rebuilt untimed).
//
//   ECS_ENTITIES (200000), ECS_REPEATS (5)

#include "bench.hpp"
#include <ecs/ecs.hpp>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <random>
#include <vector>

using namespace ecs;

struct P {
    float x, y, z;
};
struct V {
    float x, y, z;
};

namespace {

constexpr int kCells = 32; // per axis: 32^2 buckets, mist-like density

std::uint32_t cell_of(P const& p) {
    auto const ax = [](float v) {
        return std::uint32_t(v) & (kCells - 1);
    };
    return ax(p.x) * kCells + ax(p.y);
}

void run_setup(World& w, auto fn) {
    Schedule s;
    s.add_once("setup", std::move(fn));
    s.run(w);
}

// A world of n birds at seeded-random positions.
void spawn_random(World& w, std::size_t n, unsigned seed) {
    std::mt19937 rng {seed};
    std::uniform_real_distribution<float> d {0.0f, float(kCells)};
    run_setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < n; ++i)
            cmd.spawn(P {d(rng), d(rng), d(rng)}, V {1, 0, 0});
    });
}

// Destroy `frac` of the rows and respawn them: swap-and-pop churn, the way
// key coherence actually decays between maintenance sorts.
void drift(World& w, std::size_t n, double frac, unsigned seed) {
    std::vector<Entity> es;
    es.reserve(n);
    w.for_each_chunk<P const>([&](std::span<Entity const> ids, chunk<P const>) {
        es.insert(es.end(), ids.begin(), ids.end());
    });
    std::mt19937 rng {seed};
    std::uniform_real_distribution<float> d {0.0f, float(kCells)};
    auto const k = std::size_t(double(es.size()) * frac);
    std::shuffle(es.begin(), es.end(), rng);
    run_setup(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < k; ++i) {
            cmd.destroy(es[i]);
            cmd.spawn(P {d(rng), d(rng), d(rng)}, V {1, 0, 0});
        }
    });
}

void report(char const* name, std::size_t n, int repeats, double ns) {
    std::printf("  %-24s %8.2f ns/row\n", name, ns);
    bench::emit(name, "ns_per_row", ns, "ns", "lower", n, 1,
                std::size_t(repeats));
}

} // namespace

int main() {
    bench::set_suite("sort");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 200'000);
    int const R         = int(bench::env_long("ECS_REPEATS", 5));
    auto const key      = [](P const& p) { return cell_of(p); };

    std::printf("sort_bench: %zu rows, %d^2 cells, best of %d\n",
                N, kCells, R);

    // sorted scan: second sort right after the first -- keys pass, no permute
    {
        std::optional<World> w;
        double const ns = bench::best_ns_of(
            N, R,
            [&] {
                w.emplace();
                spawn_random(*w, N, 1u);
                w->sort_rows<P>(key);
            },
            [&] { w->sort_rows<P>(key); });
        report("sorted scan", N, R, ns);
    }

    // shuffled sort: random layout, full counting sort + permutation
    {
        std::optional<World> w;
        unsigned seed = 1;
        double const ns = bench::best_ns_of(
            N, R,
            [&] {
                w.emplace();
                spawn_random(*w, N, seed++);
            },
            [&] { w->sort_rows<P>(key); });
        report("shuffled sort", N, R, ns);
    }

    // drifted 5%: once with the threshold above the drift (skip), once at 0
    for (double const min_disorder : {0.2, 0.0}) {
        std::optional<World> w;
        unsigned seed = 100;
        double const ns = bench::best_ns_of(
            N, R,
            [&] {
                w.emplace();
                spawn_random(*w, N, seed);
                w->sort_rows<P>(key);
                drift(*w, N, 0.05, seed++);
            },
            [&] { w->sort_rows<P>(key, min_disorder); });
        report(min_disorder > 0 ? "5% drift, skip @0.2" : "5% drift, sort @0",
               N, R, ns);
    }
    return 0;
}
