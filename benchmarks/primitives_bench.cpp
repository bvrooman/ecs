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
// Every case reports mean us/tick (distribution rows in ECS_BENCH_OUT) for
// the imperative baseline at 1 lane and the kernel at 1 and hw lanes. The
// 1-lane kernel column IS the primitive's overhead; the hw column is the
// payoff.
//
//   ECS_ENTITIES (200000), ECS_TICKS (300)

#include "bench.hpp"
#include "ecs/ecs.hpp"
#include <cstdint>
#include <cstdio>
#include <thread>
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

unsigned hw_lanes() {
    return std::max(2u, std::thread::hardware_concurrency());
}

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

void report(char const* name, std::size_t n, int ticks, double base1,
            double k1, double khw, unsigned hw) {
    // Both bracketed numbers are SPEEDUP vs the imperative baseline
    // (imperative_time / kernel_time), same direction as the CSV's
    // kernel_speedup: >1 = the kernel is faster, <1 = slower. The x1 column
    // is thus the primitive's raw overhead (how much the slot/partial/barrier
    // machinery costs before any lanes help); the x%u column is the payoff.
    std::printf("  %-10s imperative %8.1f us | kernel x1 %8.1f us (%.2fx) | "
                "kernel x%u %8.1f us (%.2fx)\n",
                name, base1, k1, base1 / k1, hw, khw, base1 / khw);
    auto const t = std::size_t(ticks);
    bench::emit(name, "imperative_us_per_tick", base1, "us", "lower", n, 1, t);
    bench::emit(name, "kernel_us_per_tick", k1, "us", "lower", n, 1, t);
    bench::emit(name, "kernel_us_per_tick", khw, "us", "lower", n, hw, t);
    bench::emit(name, "kernel_speedup", base1 / k1, "x", "higher", n, 1, t);
    bench::emit(name, "kernel_speedup", base1 / khw, "x", "higher", n, hw, t);
}

} // namespace

int main() {
    bench::set_suite("primitives");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 200'000);
    int const T         = int(bench::env_long("ECS_TICKS", 300));
    unsigned const hw   = hw_lanes();

    std::printf("primitives_bench: %zu entities, %d ticks/config, hw=%u\n",
                N, T, hw);

    // --- reduce --------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Sum>(); };
        auto imperative = [](Schedule& s) {
            s.add("sum", [](Query<P const> q, ResMut<Sum> out) {
                out->v = 0;
                q.for_each_chunk([&](std::span<Entity>, chunk<P const> p) {
                    double acc = 0;
                    for (float x : p.column<0>())
                        acc += x;
                    out->v += acc;
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("sum", [](Query<P const> q, Reduce<Sum, AddSum> r) {
                q.for_each_chunk([&](std::span<Entity>, chunk<P const> p) {
                    double acc = 0;
                    for (float x : p.column<0>())
                        acc += x;
                    r->v += acc;
                });
            });
        };
        report("reduce", N, T, time_case(N, T, 1, setup, imperative),
               time_case(N, T, 1, setup, kernel),
               time_case(N, T, hw, setup, kernel), hw);
    }

    // --- extract ---------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Snapshot>(); };
        auto imperative = [](Schedule& s) {
            s.add("snap", [](Query<P const> q, ResMut<Snapshot> out) {
                out->clear();
                q.for_each_chunk([&](std::span<Entity>, chunk<P const> p) {
                    auto x = p.column<0>();
                    out->insert(out->end(), x.begin(), x.end());
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("snap", [](Query<P const> q, Extract<Snapshot> out) {
                q.for_each_chunk([&](std::span<Entity>, chunk<P const> p) {
                    auto x = p.column<0>();
                    for (std::size_t i = 0; i < x.size(); ++i)
                        out[i] = x[i];
                });
            });
        };
        report("extract", N, T, time_case(N, T, 1, setup, imperative),
               time_case(N, T, 1, setup, kernel),
               time_case(N, T, hw, setup, kernel), hw);
    }

    // --- collect ---------------------------------------------------------------
    {
        auto setup = [](World& w) { w.emplace_resource<Hits>(); };
        auto imperative = [](Schedule& s) {
            s.add("hits", [](Query<P const> q, ResMut<Hits> out) {
                out->clear();
                q.for_each_serial([&](auto& p) {
                    if (p.y > 156.0f) // ~half of y in [0, 313)
                        out->push_back(p.x);
                });
            });
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("hits", [](Query<P const> q, Collect<Hits> out) {
                q.for_each_serial([&](auto& p) {
                    if (p.y > 156.0f)
                        out->push_back(p.x);
                });
            });
        };
        report("collect", N, T, time_case(N, T, 1, setup, imperative),
               time_case(N, T, 1, setup, kernel),
               time_case(N, T, hw, setup, kernel), hw);
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
                q.for_each_serial([&](auto& p) {
                    out->v[std::uint32_t(p.x) & 63].push_back(p.z);
                });
            });
        };
        auto setup_k = [](World& w) {
            w.emplace_resource<Bins<float>>().reserve_buckets(64);
        };
        auto kernel = [](Schedule& s) {
            s.add_kernel("bin", [](Query<P const> q, Bin<float> out) {
                q.for_each_serial([&](auto& p) {
                    out.emit(std::uint32_t(p.x) & 63, p.z);
                });
            });
        };
        report("bin", N, T, time_case(N, T, 1, setup_imp, imperative),
               time_case(N, T, 1, setup_k, kernel),
               time_case(N, T, hw, setup_k, kernel), hw);
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
                q.for_each_serial([&](auto&) {
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
                q.for_each_serial([&](auto&) {
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
        report("events", N, T, time_case(N, T, 1, setup_imp, imperative),
               time_case(N, T, 1, setup_k, kernel),
               time_case(N, T, hw, setup_k, kernel), hw);
    }
    return 0;
}
