// SoA layout microbenchmarks.
//
// Measures the structure-of-arrays storage against a plain array-of-structures
// baseline, and the two query iteration paths (for_each vs for_each_chunk)
// against each other, across representative ECS workloads:
//
//   * integrate         -- one written component, one read-only component
//   * multi read+write  -- two written components plus one read-only
//   * single-field read -- touch only one field of one component
//   * compute-bound     -- many independent FLOPs per entity
//   * write-back cost   -- a read-only component left non-const (scattered back)
//
// Each iteration path runs as a real (single-lane) system via bench_system, so
// the numbers are the production execution path; a single serial system adds
// only O(1) scheduler overhead per run against the O(entities) kernel, so the
// layout / access-shape signal is what moves. Build in Release (-O3
// -march=native is applied below) and run:
//
//   ./build/benchmarks/soa_bench [entities] [iters] [repeats]
//
// Numbers are ns per entity per sweep; lower is better. Ratios are stable
// run-to-run, absolute values less so (they track the host's memory subsystem).

#include "bench_ecs.hpp"
#include <cstdlib>
#include <span>
#include <vector>

using namespace ecs;

struct Pos {
    float x, y, z;
};
struct Vel {
    float x, y, z;
};
struct Acc {
    float x, y, z;
};

// A branch-free, division-free kernel with a per-entity dependency chain but no
// cross-entity dependency -- representative of nontrivial per-entity math.
inline static float heavy(float a, float b, float c) {
    float r = a;

    r = r * 0.99f + b;
    r = r * 0.98f + c;
    r = r * 0.97f + b;
    r = r * 0.96f + c;
    r = r * 0.95f + b;
    r = r * 0.94f + c;
    r = r * 0.93f + b;
    r = r * 0.92f + c;
    r = r * 0.91f + b;
    r = r * 0.90f + c;
    r = r * 0.89f + b;
    r = r * 0.88f + c;
    r = r * 0.87f + b;
    r = r * 0.86f + c;
    r = r * 0.85f + b;
    r = r * 0.84f + c;
    return r;
}

int main(int argc, char** argv) {
    bench::set_suite("soa");
    std::size_t const N = argc > 1 ? std::strtoull(argv[1], nullptr, 10)
                                   : bench::env_size("ECS_ENTITIES", 1'000'000);
    bench::g_iters      = int(bench::env_long("ECS_ITERS", bench::g_iters));
    bench::g_repeats    = int(bench::env_long("ECS_REPEATS", bench::g_repeats));
    if (argc > 2)
        bench::g_iters = std::atoi(argv[2]);
    if (argc > 3)
        bench::g_repeats = std::atoi(argv[3]);

    // --- populate the ECS world (one entity = {Pos, Vel, Acc}) ---------------
    World w;
    Schedule init;
    init.add_serial(
        "populate",
        [N](Commands& cmd) {
            for (std::size_t i = 0; i < N; ++i)
                cmd.spawn(Pos {float(i), 0, 0}, Vel {1, 1, 1}, Acc {0.01f, 0, 0});
        },
        times {1});
    init.run(w);

    // --- an equivalent array-of-structures baseline --------------------------
    struct Body {
        Pos p;
        Vel v;
        Acc a;
    };
    std::vector<Body> aos(N);
    for (std::size_t i = 0; i < N; ++i)
        aos[i] = {{float(i), 0, 0}, {1, 1, 1}, {0.01f, 0, 0}};

    std::printf("SoA layout benchmark: %zu entities, best of %d x %d sweeps\n",
                N,
                bench::g_repeats,
                bench::g_iters);

    // 1/2) integrate: write Pos, read Vel ------------------------------------
    bench_system("for_each integrate (1 RW, 1 RO)", N, w, [](Query<Pos, Vel const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x;
            p.y += v.y;
            p.z += v.z;
        });
    });
    bench_system("for_each_chunk integrate (1 RW, 1 RO)",
                 N,
                 w,
                 [](Query<Pos, Vel const> q) {
                     q.for_each_chunk([](std::span<Entity const>,
                                         chunk<Pos> p,
                                         chunk<Vel const> v) {
                         auto px = p.column<0>(), py = p.column<1>(), pz = p.column<2>();
                         auto const vx = v.column<0>();
                         auto const vy = v.column<1>();
                         auto const vz = v.column<2>();
                         for (std::size_t i = 0; i < px.size(); ++i) {
                             px[i] += vx[i];
                             py[i] += vy[i];
                             pz[i] += vz[i];
                         }
                     });
                 });

    // 3/4/5) multi read+write: write Pos and Vel, read Acc; vs AoS ------------
    bench_system("for_each multi (2 RW, 1 RO)", N, w, [](Query<Pos, Vel, Acc const> q) {
        q.for_each([](auto& p, auto& v, auto& a) {
            v.x += a.x;
            v.y += a.y;
            v.z += a.z;
            p.x += v.x;
            p.y += v.y;
            p.z += v.z;
        });
    });
    bench_system("for_each_chunk multi (2 RW, 1 RO)",
                 N,
                 w,
                 [](Query<Pos, Vel, Acc const> q) {
                     q.for_each_chunk([](std::span<Entity const>,
                                         chunk<Pos> p,
                                         chunk<Vel> v,
                                         chunk<Acc const> a) {
                         auto px = p.column<0>(), py = p.column<1>(), pz = p.column<2>();
                         auto vx = v.column<0>(), vy = v.column<1>(), vz = v.column<2>();
                         auto ax = a.column<0>(), ay = a.column<1>(), az = a.column<2>();
                         for (std::size_t i = 0; i < px.size(); ++i) {
                             vx[i] += ax[i];
                             vy[i] += ay[i];
                             vz[i] += az[i];
                             px[i] += vx[i];
                             py[i] += vy[i];
                             pz[i] += vz[i];
                         }
                     });
                 });
    bench::run("AoS baseline   multi (2 RW, 1 RO)", N, [&] {
        for (auto& b : aos) {
            b.v.x += b.a.x;
            b.v.y += b.a.y;
            b.v.z += b.a.z;
            b.p.x += b.v.x;
            b.p.y += b.v.y;
            b.p.z += b.v.z;
        }
    });

    // 6) single-field read: sum only Pos.x (read-only, no system) -------------
    bench::run("for_each_chunk sum 1 field (SoA)", N, [&] {
        float s = 0;
        w.for_each_chunk<Pos const>([&](std::span<Entity const>, chunk<Pos const> p) {
            for (float x : p.column<0>())
                s += x;
        });
        bench::keep(s);
    });
    bench::run("AoS            sum 1 field", N, [&] {
        float s = 0;
        for (auto& b : aos)
            s += b.p.x;
        bench::keep(s);
    });

    // 7/8) compute-bound: many FLOPs per entity ------------------------------
    bench_system("for_each compute-bound", N, w, [](Query<Pos, Vel const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x = heavy(p.x, v.x, v.y);
            p.y = heavy(p.y, v.y, v.z);
            p.z = heavy(p.z, v.z, v.x);
        });
    });
    bench_system("for_each_chunk compute-bound", N, w, [](Query<Pos, Vel const> q) {
        q.for_each_chunk([](std::span<Entity const>, chunk<Pos> p, chunk<Vel const> v) {
            auto px = p.column<0>(), py = p.column<1>(), pz = p.column<2>();
            auto vx = v.column<0>(), vy = v.column<1>(), vz = v.column<2>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] = heavy(px[i], vx[i], vy[i]);
                py[i] = heavy(py[i], vy[i], vz[i]);
                pz[i] = heavy(pz[i], vz[i], vx[i]);
            }
        });
    });

    // 9) write-back cost: read Vel but leave it non-const (scattered back) ----
    bench_system("for_each read Vel NON-const (write-back)", N, w, [](Query<Pos, Vel> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x;
            p.y += v.y;
            p.z += v.z;
        });
    });
    bench_system("for_each read Vel const (no write-back)",
                 N,
                 w,
                 [](Query<Pos, Vel const> q) {
                     q.for_each([](auto& p, auto& v) {
                         p.x += v.x;
                         p.y += v.y;
                         p.z += v.z;
                     });
                 });

    return 0;
}
