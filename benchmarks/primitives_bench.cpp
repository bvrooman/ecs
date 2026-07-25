// benchmarks/primitives_bench.cpp
//
// The kernel parallel primitives against hand-rolled imperative baselines:
// what does the slot/partial/barrier machinery cost at 1 lane, and what does
// it buy at hw lanes? Each case is the primitive's canonical shape:
//
//   * reduce   -- Reduce<Sum>: fold a float per row into one resource
//                 vs an imperative ResMut serial accumulation
//   * extract  -- Extract<Snapshot>: disjoint-span gather of one field
//                 vs an imperative resize + serial write
//   * collect  -- Collect<Hits>: filtered gather (~half the rows pass)
//                 vs an imperative clear + push_back walk
//   * bin      -- Bin<float>: group rows into 64 buckets (counting sort at
//                 the barrier) vs an imperative bucket-vector rebuild
//   * events   -- EventWriter/Reader roundtrip (K events/tick) vs a ResMut
//                 vector rebuilt each tick
//
// Every case reports mean us/tick (distribution rows in ECS_BENCH_OUT) for the
// imperative baseline at 1 lane and the kernel swept across ECS_LANES: the
// 1-lane kernel point is the primitive's raw overhead, and the curve across
// lanes is the payoff -- including where it turns over past the physical-core
// count. Default lane set is 1,2,4,...,hw (see bench::lane_set).
//
//   ECS_ENTITIES (200000), ECS_TICKS (300), ECS_LANES ("1,2,4,8")

#include "bench.hpp"
#include "ecs/ecs.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace ecs;

struct P {
    float x, y, z;
};

// reduce: one scalar sum.
struct Sum {
    double v = 0;
};
struct AddSum {
    void operator()(Sum& into, Sum& part) const { into.v += part.v; }
};

// extract / collect targets.
using Snapshot = std::vector<float>;
using Hits     = std::vector<float>;

// events payload.
struct Ping {
    std::uint32_t v;
};

namespace {

void populate(World& w, std::size_t n) {
    Schedule s;
    s.add_once("seed", [n](Commands& cmd) {
        for (std::size_t i = 0; i < n; ++i)
            cmd.spawn(P {float(i % 977), float(i % 313), float(i)});
    });
    s.run(w);
}

// Run `build`'s one-system schedule at `lanes`, report mean us/tick.
template <class Build, class Setup>
double time_case(std::size_t n, int ticks, unsigned lanes, Setup&& setup,
                 Build&& build) {
    World w;
    setup(w);
    populate(w, n);
    Schedule s;
    build(s);
    WorkerPool pool {lanes};
    return bench::measure_ticks(std::max(20, ticks / 5), ticks,
                                [&] { s.run(w, pool); })
        .mean;
}

// Imperative serial baseline + a kernel LANE SWEEP, printed and emitted. The
// per-lane bracket is speedup vs the imperative baseline (>1 = kernel faster),
// matching the CSV's kernel_speedup: the x1 point is the primitive's raw
// overhead (the slot/partial/barrier machinery before any lanes help), and the
// curve across lanes is the payoff -- including where it turns over. si/sk are
// the setups (they differ for bin/events); imp/ker build the one-system
// schedule.
template <class SetupI, class Imp, class SetupK, class Ker>
void run_primitive(char const* name, std::size_t n, int ticks,
                   std::vector<unsigned> const& lanes, SetupI&& si, Imp&& imp,
                   SetupK&& sk, Ker&& ker) {
    auto const t      = std::size_t(ticks);
    double const base = time_case(n, ticks, 1, si, imp);
    std::printf("  %-10s imperative %8.1f us |", name, base);
    bench::emit(name, "imperative_us_per_tick", base, "us", "lower", n, 1, t);
    for (unsigned L : lanes) {
        double const k = time_case(n, ticks, L, sk, ker);
        std::printf(" x%u %7.1f (%.2fx)", L, k, base / k);
        bench::emit(name, "kernel_us_per_tick", k, "us", "lower", n, L, t);
        bench::emit(name, "kernel_speedup", base / k, "x", "higher", n, L, t);
    }
    std::printf("\n");
}

} // namespace

int main() {
    bench::set_suite("primitives");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 200'000);
    int const T          = int(bench::env_long("ECS_TICKS", 300));
    auto const lanes     = bench::lane_set();

    std::printf("primitives_bench: %zu entities, %d ticks/config, lanes",
                N, T);
    for (unsigned L : lanes)
        std::printf(" %u", L);
    std::printf("\n");

    // --- reduce --------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Sum>(); };
        auto imperative = [](Schedule& s) {
            s.add("sum", [](Query<P const> q, ResMut<Sum> out) {
                out->v = 0;
                q.for_each_chunk([&](std::span<Entity const>, chunk<P const> p) {
                    double acc = 0;
                    for (float x : p.column<0>())
                        acc += x;
                    out->v += acc;
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("sum", [](Query<P const> q, Reduce<Sum, AddSum> r) {
                q.for_each_chunk([&](std::span<Entity const>, chunk<P const> p) {
                    double acc = 0;
                    for (float x : p.column<0>())
                        acc += x;
                    r->v += acc;
                });
            });
        };
        run_primitive("reduce", N, T, lanes, setup, imperative, setup, kernel);
    }

    // --- extract ---------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Snapshot>(); };
        auto imperative = [](Schedule& s) {
            s.add("snap", [](Query<P const> q, ResMut<Snapshot> out) {
                out->clear();
                q.for_each_chunk([&](std::span<Entity const>, chunk<P const> p) {
                    auto x = p.column<0>();
                    out->insert(out->end(), x.begin(), x.end());
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("snap", [](Query<P const> q, Extract<Snapshot> out) {
                q.for_each_chunk([&](std::span<Entity const>, chunk<P const> p) {
                    auto x = p.column<0>();
                    for (std::size_t i = 0; i < x.size(); ++i)
                        out[i] = x[i];
                });
            });
        };
        run_primitive("extract", N, T, lanes, setup, imperative, setup, kernel);
    }

    // --- collect ---------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Hits>(); };
        auto imperative = [](Schedule& s) {
            s.add("hits", [](Query<P const> q, ResMut<Hits> out) {
                out->clear();
                q.for_each([&](auto& p) {
                    if (p.y > 156.0f) // ~half of y in [0, 313)
                        out->push_back(p.x);
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("hits", [](Query<P const> q, Collect<Hits> out) {
                q.for_each([&](auto& p) {
                    if (p.y > 156.0f)
                        out->push_back(p.x);
                });
            });
        };
        run_primitive("collect", N, T, lanes, setup, imperative, setup, kernel);
    }

    // --- bin -------------------------------------------------------------------
    {
        // Imperative baseline: the classic bucket-vector rebuild.
        struct Buckets {
            std::vector<std::vector<float>> v;
        };
        auto setup_imp = [](World& w) {
            w.emplace_resource<Buckets>(Buckets {
                std::vector<std::vector<float>>(64)});
        };
        auto imperative = [](Schedule& s) {
            s.add("bin", [](Query<P const> q, ResMut<Buckets> out) {
                for (auto& b : out->v)
                    b.clear();
                q.for_each([&](auto& p) {
                    out->v[std::uint32_t(p.x) & 63].push_back(p.z);
                });
            });
        };
        auto setup_k = [](World& w) {
            w.emplace_resource<Bins<float>>().reserve_buckets(64);
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("bin", [](Query<P const> q, Bin<float> out) {
                q.for_each([&](auto& p) {
                    out.emit(std::uint32_t(p.x) & 63, p.z);
                });
            });
        };
        run_primitive("bin", N, T, lanes, setup_imp, imperative, setup_k, kernel);
    }

    // --- events ------------------------------------------------------------------
    // Writer emits ~N/20 messages per tick, reader folds them. The imperative
    // baseline is what you'd write without the channel: a plain vector
    // rebuilt each tick, reader ordered after the writer by the ResMut/Res
    // conflict. The kernel form emits through EventWriter (per-item partials,
    // barrier concat; readable next tick by the double-buffer contract).
    {
        struct PingBuf {
            std::vector<Ping> v;
        };
        auto setup_imp = [](World& w) {
            w.emplace_resource<PingBuf>();
            w.emplace_resource<Sum>();
        };
        auto imperative = [](Schedule& s) {
            s.add("write", [](Query<P const> q, ResMut<PingBuf> out) {
                out->v.clear();
                std::uint32_t i = 0;
                q.for_each([&](auto&) {
                    if (i % 20 == 0)
                        out->v.push_back(Ping {i});
                    ++i;
                });
            });
            s.add("read", [](Res<PingBuf> ch, ResMut<Sum> sum) {
                double acc = 0;
                for (auto const& e : ch->v)
                    acc += e.v;
                sum->v = acc;
            });
        };
        auto setup_k = [](World& w) {
            w.emplace_resource<Events<Ping>>();
            w.emplace_resource<Sum>();
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("write", [](Query<P const> q, EventWriter<Ping> out) {
                std::uint32_t i = 0;
                q.for_each([&](auto&) {
                    if (i % 20 == 0)
                        out.emit(Ping {i});
                    ++i;
                });
            });
            s.add("read", [](Res<Events<Ping>> ch, ResMut<Sum> sum) {
                double acc = 0;
                for (auto const& e : ch->read())
                    acc += e.v;
                sum->v = acc;
            });
        };
        run_primitive("events", N, T, lanes, setup_imp, imperative, setup_k,
                      kernel);
    }
    return 0;
}
