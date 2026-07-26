// benchmarks/churn_bench.cpp
//
// Structural-churn costs: the paths sort_rows, the edge cache, and the
// command buffer live on, which the iteration suites never touch.
//
//   * spawn        -- N spawns recorded + flushed into their final archetype
//   * despawn      -- N destroys (swap-and-pop) recorded + flushed
//   * add/remove   -- component add/remove on N entities (archetype moves)
//   * steady churn -- emitter/reaper steady state (K spawns + K destroys per
//                     tick over a stable population): us/tick distribution +
//                     allocs/tick, the zero-allocation goal under churn
//
// One-shot cases report best-of-R ns per operation (setup rebuilt untimed
// each repeat); steady churn reports the per-tick distribution.
//
//   ECS_ENTITIES (100000), ECS_REPEATS (5), ECS_TICKS (2000, steady churn)

#include <ecs/ecs.hpp>

#include "bench.hpp"
#include "bench_alloc.hpp"
#include <cstdio>
#include <deque>
#include <optional>
#include <vector>

using namespace ecs;

struct P {
    float x, y, z;
};
struct V {
    float x, y, z;
};
struct Extra {
    float v;
};

namespace {

void run_setup(World& w, auto fn) {
    Schedule s;
    s.add_once("setup", std::move(fn));
    s.run(w);
}

// Every live entity, in serial-walk order.
std::vector<Entity> live_entities(World& w) {
    std::vector<Entity> es;
    es.reserve(w.size());
    w.for_each_chunk<P const>([&](std::span<Entity const> ids, chunk<P const>) {
        es.insert(es.end(), ids.begin(), ids.end());
    });
    return es;
}

void report(char const* name, std::size_t n, int repeats, double ns) {
    std::printf("  %-28s %8.1f ns/op\n", name, ns);
    bench::emit(name, "ns_per_op", ns, "ns", "lower", n, 1, std::size_t(repeats));
}

} // namespace

int main() {
    bench::set_suite("churn");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 100'000);
    int const R         = int(bench::env_long("ECS_REPEATS", 5));
    int const ticks     = int(bench::env_long("ECS_TICKS", 2000));

    std::printf("churn_bench: %zu entities, best of %d\n", N, R);

    // --- spawn: record N + flush into the final archetype -------------------
    {
        std::optional<World> w;
        double const ns = bench::best_ns_of(
            N,
            R,
            [&] { w.emplace(); },
            [&] {
                run_setup(*w, [&](Commands& cmd) {
                    for (std::size_t i = 0; i < N; ++i)
                        cmd.spawn(P {float(i), 0, 0}, V {1, 1, 1});
                });
            });
        report("spawn {P,V}", N, R, ns);
    }

    // --- despawn: destroy the whole population ------------------------------
    {
        std::optional<World> w;
        std::vector<Entity> es;
        double const ns = bench::best_ns_of(
            N,
            R,
            [&] {
                w.emplace();
                run_setup(*w, [&](Commands& cmd) {
                    for (std::size_t i = 0; i < N; ++i)
                        cmd.spawn(P {float(i), 0, 0}, V {1, 1, 1});
                });
                es = live_entities(*w);
            },
            [&] {
                run_setup(*w, [&](Commands& cmd) {
                    for (Entity e : es)
                        cmd.destroy(e);
                });
            });
        report("despawn {P,V}", N, R, ns);
    }

    // --- add component: {P,V} -> {P,V,Extra} (archetype move) ---------------
    {
        std::optional<World> w;
        std::vector<Entity> es;
        double const ns = bench::best_ns_of(
            N,
            R,
            [&] {
                w.emplace();
                run_setup(*w, [&](Commands& cmd) {
                    for (std::size_t i = 0; i < N; ++i)
                        cmd.spawn(P {float(i), 0, 0}, V {1, 1, 1});
                });
                es = live_entities(*w);
            },
            [&] {
                run_setup(*w, [&](Commands& cmd) {
                    for (Entity e : es)
                        cmd.add(e, Extra {1.0f});
                });
            });
        report("add Extra (move)", N, R, ns);
    }

    // --- remove component: {P,V,Extra} -> {P,V} ------------------------------
    {
        std::optional<World> w;
        std::vector<Entity> es;
        double const ns = bench::best_ns_of(
            N,
            R,
            [&] {
                w.emplace();
                run_setup(*w, [&](Commands& cmd) {
                    for (std::size_t i = 0; i < N; ++i)
                        cmd.spawn(P {float(i), 0, 0}, V {1, 1, 1}, Extra {1.0f});
                });
                es = live_entities(*w);
            },
            [&] {
                run_setup(*w, [&](Commands& cmd) {
                    for (Entity e : es)
                        cmd.remove<Extra>(e);
                });
            });
        report("remove Extra (move)", N, R, ns);
    }

    // --- steady churn: emitter/reaper over a stable population --------------
    // K spawns + K destroys of the oldest per tick (K = N/100), plus one
    // touch-everything system so the churn shares the tick with real work.
    {
        std::size_t const K = std::max<std::size_t>(1, N / 100);
        struct Ring {
            std::deque<Entity> fifo; // spawn order: front = oldest
        };
        World w;
        w.emplace_resource<Ring>();
        run_setup(w, [&](Commands& cmd) {
            for (std::size_t i = 0; i < N; ++i)
                cmd.spawn(P {float(i), 0, 0}, V {1, 1, 1});
        });
        auto const es = live_entities(w);
        w.resource<Ring>().fifo.assign(es.begin(), es.end());

        Schedule s;
        s.add("churn", [K](Commands& cmd, ResMut<Ring> ring) {
            for (std::size_t i = 0; i < K; ++i) {
                cmd.destroy(ring->fifo.front());
                ring->fifo.pop_front();
                ring->fifo.push_back(cmd.spawn(P {0, 0, 0}, V {1, 1, 1}));
            }
        });
        s.add("integrate", [](Query<P, V const> q) {
            q.for_each_chunk([](std::span<Entity const>, chunk<P> p, chunk<V const> v) {
                auto px = p.column<0>();
                auto vx = v.column<0>();
                for (std::size_t i = 0; i < px.size(); ++i)
                    px[i] += vx[i];
            });
        });

        bench::warmup(std::max(50, ticks / 10), [&] { s.run(w); });
        bench::Dist st;
        double const allocs = bench::count_allocs(ticks, [&] {
            st = bench::measure_ticks(0, ticks, [&] { s.run(w); });
        });
        std::printf("  steady churn (K=%zu/tick)      mean %7.1f  p50 %7.1f  "
                    "p99 %8.1f us | %5.1f alloc/tick\n",
                    K,
                    st.mean,
                    st.p50,
                    st.p99,
                    allocs);
        bench::emit_dist("steady_churn", 1, st, N, std::size_t(ticks));
        bench::emit("steady_churn",
                    "allocs_per_tick",
                    allocs,
                    "1",
                    "lower",
                    N,
                    1,
                    std::size_t(ticks));
    }
    return 0;
}
