// gather_bench: the cost of for_each_parallel's per-row gather/scatter as a
// component grows wide or gains a non-trivially-copyable field -- vs for_each_chunk
// (raw column spans) on the same backend, and vs the P2996 proxy path.
//
// On the PORTABLE backend (no reflected field names), for_each_parallel gathers the
// WHOLE component into a local and scatters the mutable fields back, regardless of
// which fields the kernel touches. for_each_chunk -- and, on P2996, the zero-copy
// row proxy -- touch only the columns the kernel accesses. So for a wide component
// read/written SPARSELY, or one carrying a heap field (e.g. std::string), the
// portable gather cost explodes, while the P2996 path stays flat.
//
//   P2996 build (default):  proxies -> ~1.0x everywhere (the gather cost is P2996-free)
//   portable build:         -DECS_USE_P2996=0 (drop -freflection) -> the gather blowup
//
// e.g. reconfigure a portable build dir with -DECS_USE_P2996=OFF, or:
//   g++ -std=c++23 -DECS_USE_P2996=0 -O3 -march=native -Iinclude \
//       benchmarks/gather_bench.cpp -pthread -o gather_bench
//
// Single lane throughout: this isolates the per-element gather/scatter cost, not
// the pool. All kernels write component state, so the work is not elided.

#include "bench.hpp"
#include "ecs/ecs.hpp"
#include <chrono>
#include <cstdio>
#include <string>

using namespace ecs;

struct Vel {
    float x, y, z;
};
struct W3 {
    float a, b, c;
};
struct W8 {
    float a, b, c, d, e, f, g, h;
};
struct W16 {
    float a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p;
};
struct Named {
    std::string tag;
    float v;
}; // a non-trivially-copyable field

namespace {
using clk = std::chrono::steady_clock;
template <class Fn>
void populate(World& w, Fn fn) {
    Schedule s;
    s.add_once("seed", std::move(fn));
    s.run(w);
}
// Timing uses the shared bench::min_ns_per (bench.hpp) -- same estimator
// as every other benchmark here.
using bench::min_ns_per;
void report(char const* label, std::size_t n, int repeats, double ch, double par) {
    std::printf("  %-42s chunk %8.3f ns   parallel %9.3f ns   %7.2fx\n",
                label,
                ch,
                par,
                par / ch);
    auto const r = std::size_t(repeats);
    bench::emit(label, "chunk_ns_per_entity", ch, "ns", "lower", n, 1, r);
    bench::emit(label, "parallel_ns_per_entity", par, "ns", "lower", n, 1, r);
    bench::emit(label, "parallel_over_chunk", par / ch, "x", "lower", n, 1, r);
}
} // namespace

// A wide component whose kernel touches only its first field (+ Vel.x).
template <class W>
static void sparse(char const* label, std::size_t N, int R) {
    World w;
    populate(w, [&](Commands& cmd) {
        for (std::size_t i = 0; i < N; ++i) {
            W ww {};
            ww.a = float(i);
            cmd.spawn(ww, Vel {0.1f, 0.2f, 0.3f});
        }
    });
    WorkerPool pool {1};
    double const ch  = min_ns_per(N, R, [&] {
        query<W, Vel const>(w).for_each_chunk([](std::span<Entity>,
                                                 chunk<W> p,
                                                 chunk<Vel const> v) {
            auto a  = p.template column<0>();
            auto vx = v.column<0>();
            for (std::size_t i = 0; i < a.size(); ++i)
                a[i] += vx[i];
        });
    });
    double const par = min_ns_per(N, R, [&] {
        Query<W, Vel const>(w, pool).for_each_parallel([](auto& p, auto& v) {
            p.a += v.x;
        });
    });
    report(label, N, R, ch, par);
}

int main() {
    bench::set_suite("gather");
    std::size_t const N = bench::env_size("ECS_ENTITIES", 200'000);
    int const R         = int(bench::env_long("ECS_REPEATS", 250));
#if ECS_USE_P2996
    std::printf("gather_bench -- P2996 backend (for_each_parallel = row PROXIES)\n");
#else
    std::printf(
        "gather_bench -- portable backend (for_each_parallel = GATHER/SCATTER)\n");
#endif
    std::printf(
        "N=%zu, 1 lane. Sparse kernel = component has many fields, kernel touches 1:\n",
        N);
    sparse<W3>("W3   (3 fields, touch 1)", N, R);
    sparse<W8>("W8   (8 fields, touch 1)", N, R);
    sparse<W16>("W16  (16 fields, touch 1)", N, R);

    // Contrast: W16 with a kernel that touches ALL 16 fields -- no wasted gather.
    {
        World w;
        populate(w, [&](Commands& cmd) {
            for (std::size_t i = 0; i < N; ++i)
                cmd.spawn(W16 {}, Vel {0.1f, 0.2f, 0.3f});
        });
        WorkerPool pool {1};
        double const ch  = min_ns_per(N, R, [&] {
            query<W16, Vel const>(w).for_each_chunk([](std::span<Entity>,
                                                       chunk<W16> p,
                                                       chunk<Vel const> v) {
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    auto cols = std::tuple {p.template column<I>()...};
                    auto vx   = v.column<0>();
                    for (std::size_t i = 0; i < vx.size(); ++i)
                        ((std::get<I>(cols)[i] += vx[i]), ...);
                }(std::make_index_sequence<16> {});
            });
        });
        double const par = min_ns_per(N, R, [&] {
            Query<W16, Vel const>(w, pool).for_each_parallel([](auto& p, auto& v) {
                p.a += v.x;
                p.b += v.x;
                p.c += v.x;
                p.d += v.x;
                p.e += v.x;
                p.f += v.x;
                p.g += v.x;
                p.h += v.x;
                p.i += v.x;
                p.j += v.x;
                p.k += v.x;
                p.l += v.x;
                p.m += v.x;
                p.n += v.x;
                p.o += v.x;
                p.p += v.x;
            });
        });
        report("W16  (16 fields, touch ALL 16)", N, R, ch, par);
    }

    // Non-trivially-copyable field: a heap std::string the kernel never touches.
    {
        World w;
        std::string const big = "a_long_entity_name_that_exceeds_the_sso_buffer";
        populate(w, [&](Commands& cmd) {
            for (std::size_t i = 0; i < N; ++i)
                cmd.spawn(Named {big, float(i)}, Vel {0.1f, 0.2f, 0.3f});
        });
        WorkerPool pool {1};
        double const ch  = min_ns_per(N, R, [&] {
            query<Named, Vel const>(w).for_each_chunk([](std::span<Entity>,
                                                         chunk<Named> p,
                                                         chunk<Vel const> v) {
                auto x =
                    p.template column<1>(); // the float; the string column is untouched
                auto vx = v.column<0>();
                for (std::size_t i = 0; i < x.size(); ++i)
                    x[i] += vx[i];
            });
        });
        double const par = min_ns_per(N, R, [&] {
            Query<Named, Vel const>(w, pool).for_each_parallel([](auto& p, auto& v) {
                p.v += v.x;
            });
        });
        report("Named{std::string,float} (touch float)", N, R, ch, par);
    }
    return 0;
}
