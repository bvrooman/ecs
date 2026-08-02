// examples/nbody.cpp
//
// A real O(n^2) gravitational n-body simulation -- every body pulled by every
// other, no approximation. It exists to be the REFERENCE: the Barnes-Hut demo
// is measured against this for both accuracy and speed, so this file stays
// deliberately simple and obviously correct rather than clever.
//
// The interesting part for the library is how PAIRWISE work is expressed. A
// system may take at most one Query, and a parallel system's Query is sliced
// into per-item row ranges -- so "every body reads every other body" cannot be
// written as a query at all. The sanctioned shape (see the add_parallel
// static_assert in schedule/schedule.hpp) is to build a structure and read it
// through a resource:
//
//   gather   Extract<BodyArray>  -- disjoint per-item spans write a flat
//                                   {x,y,z,m} array straight into the resource
//   forces   Res<BodyArray>      -- each row sums over the whole array
//
// The schedule is four systems, and nothing declares an order: access alone
// levels them into gather -> forces -> integrate -> diagnostics.
//
//   gather      reads Position, Mass          writes BodyArray
//   forces      reads Position, Mass, Body..  writes Accel, Potential
//   integrate   reads Accel, Mass             writes Position, Velocity, Motion
//   diagnostics reads Potential, Motion       writes Diag
//
// Physics: Plummer-softened gravity, a_i = sum_j G m_j d_ij / (|d|^2 + eps^2)^1.5,
// advanced by semi-implicit (symplectic) Euler -- v += a dt, then x += v dt.
// Symplectic matters for a reference: energy error stays bounded and
// oscillatory instead of drifting monotonically, so "is the integrator or the
// force wrong?" has a usable answer. Softening is what keeps the inner loop
// branch-free: a close pair is bounded rather than special-cased.
//
// One caveat this demo surfaces rather than hides: the executor slices a
// parallel system into items of grain = max(kMinItemRows, rows / 64), with
// kMinItemRows = 1024 (wave.hpp). That floor is sized for cheap per-row work,
// but here per-row work is O(n) -- so below 1024 * lanes bodies there are fewer
// items than lanes and the spare lanes idle. Measured on 4 cores: 1024 bodies
// -> 1 item -> 1.00x at any lane count; 2048 -> 2 items -> 1.88x; 8192 -> 8
// items -> 3.77x. main() prints the item count and says so when it binds. A
// per-dispatch grain hint is roadmap (docs/IMPROVEMENTS.md, item 6).
//
//   ./build/examples/nbody                       # 4096 bodies, 50 ticks
//   ECS_BODIES=8192 ECS_TICKS=200 ./build/examples/nbody
//   ECS_LANES=1,2,4 ./build/examples/nbody       # lane sweep
//   ECS_BENCH_OUT=/tmp/b.csv ./build/examples/nbody
//
// Build Release for meaningful timing: cmake -DCMAKE_BUILD_TYPE=Release

#include <ecs/ecs.hpp>

#include "bench.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <span>
#include <vector>

using namespace ecs;

// --- components -------------------------------------------------------------
struct Position {
    float x, y, z;
};
struct Velocity {
    float x, y, z;
};
struct Mass {
    float kg;
};
struct Accel {
    float x, y, z;
};

// --- units ------------------------------------------------------------------
// G = 1, total mass = 1, initial radius = 1. Everything below is in those
// units, so a crossing time is ~1 and the numbers stay readable.
namespace cfg {
constexpr float kG      = 1.0f;
constexpr float kRadius = 1.0f;
constexpr float kSoft   = 0.02f; // Plummer epsilon
constexpr float kSoft2  = kSoft * kSoft;
constexpr float kDt     = 0.004f;
}

// The flat body array the force kernel sums over: the whole system, rebuilt
// every tick by `gather`. AoS (not SoA) on purpose -- the inner loop reads all
// four fields of body j together, so one 16-byte line per j is the tightest
// access pattern available.
struct Body {
    float x, y, z, m;
};
using BodyArray = std::vector<Body>;

// --- reduction targets ------------------------------------------------------
// Potential and kinetic energy land in SEPARATE resources because a Reduce
// resets its target at wave prepare: two systems folding into one resource
// would have the later reset wipe the earlier fold. Both accumulate in double
// -- summing n^2 float terms loses the drift signal we are trying to measure.
struct Potential {
    double u = 0;
};
struct AddPotential {
    void operator()(Potential& a, Potential& b) const { a.u += b.u; }
};

struct Motion {
    double ke = 0, px = 0, py = 0, pz = 0;
};
struct AddMotion {
    void operator()(Motion& a, Motion& b) const {
        a.ke += b.ke;
        a.px += b.px;
        a.py += b.py;
        a.pz += b.pz;
    }
};

// Running conservation record, updated once per tick by `diagnostics`.
// Energy drift is relative (|dE/E0|); momentum drift is absolute, since the
// seeded |P0| is ~0 by construction and a relative measure would divide by it.
struct Diag {
    double e0 = 0, e = 0, max_drift = 0;
    double p0 = 0, p = 0, max_p_drift = 0;
    bool primed         = false;
    std::uint64_t ticks = 0;
};

namespace {

int env_int(char const* key, int const dflt) { return int(bench::env_long(key, dflt)); }

// Seed a uniform-density sphere of `n` equal-mass bodies in virial equilibrium.
//
// Positions: uniform in the unit ball (cube rejection -- exact, and the draw
// order is fixed, so the same seed gives the same system every run).
// Velocities: isotropic random directions with the speed the virial theorem
// asks for. For a uniform sphere U = -(3/5) G M^2 / R analytically, and 2T =
// |U| gives <v^2> = 3 G M / (5 R) -- so no O(n^2) setup pass is needed to put
// the system in equilibrium. Starting virialized is what makes energy drift a
// meaningful signal: the system neither collapses nor evaporates, so any drift
// is the integrator, not the initial conditions relaxing.
//
// Both means are then subtracted, which is why the draws are buffered rather
// than spawned as they are generated. n random velocities carry a net momentum
// of order v_rms * sqrt(n) / n -- small, conserved, and entirely spurious: the
// whole system would coast off in that direction forever. Centring the
// positions likewise puts the centre of mass at the origin. Standard n-body
// housekeeping, and it makes |P| ~ 0 an invariant worth asserting instead of a
// random number that merely stays constant.
void seed_bodies(World& world, int const n) {
    float const m     = 1.0f / float(n);
    float const v_rms = std::sqrt(3.0f * cfg::kG / (5.0f * cfg::kRadius));

    std::vector<Position> pos;
    std::vector<Velocity> vel;
    pos.reserve(std::size_t(n));
    vel.reserve(std::size_t(n));

    std::mt19937 rng {20260802u};
    std::uniform_real_distribution<float> u {-1.0f, 1.0f};
    std::normal_distribution<float> g {0.0f, 1.0f};
    for (int i = 0; i < n; ++i) {
        float x, y, z;
        do { // rejection: uniform in the ball, not the cube
            x = u(rng);
            y = u(rng);
            z = u(rng);
        } while (x * x + y * y + z * z > 1.0f);
        // Isotropic direction: a normalized 3D Gaussian is uniform on the
        // sphere (normalizing a uniform cube draw is not).
        float dx = g(rng), dy = g(rng), dz = g(rng);
        float const len = std::sqrt(dx * dx + dy * dy + dz * dz);
        float const s   = len > 1e-6f ? v_rms / len : 0.0f;
        pos.push_back({x * cfg::kRadius, y * cfg::kRadius, z * cfg::kRadius});
        vel.push_back({dx * s, dy * s, dz * s});
    }

    // Equal masses, so the mass-weighted means are the plain means.
    double cx = 0, cy = 0, cz = 0, vx = 0, vy = 0, vz = 0;
    for (int i = 0; i < n; ++i) {
        cx += pos[std::size_t(i)].x;
        cy += pos[std::size_t(i)].y;
        cz += pos[std::size_t(i)].z;
        vx += vel[std::size_t(i)].x;
        vy += vel[std::size_t(i)].y;
        vz += vel[std::size_t(i)].z;
    }
    float const inv = 1.0f / float(n);
    float const ox = float(cx) * inv, oy = float(cy) * inv, oz = float(cz) * inv;
    float const dvx = float(vx) * inv, dvy = float(vy) * inv, dvz = float(vz) * inv;

    Schedule s;
    s.add_serial(
        "seed",
        [&, n, m](Commands& cmd) {
            for (int i = 0; i < n; ++i) {
                auto const& p = pos[std::size_t(i)];
                auto const& v = vel[std::size_t(i)];
                cmd.spawn(Position {p.x - ox, p.y - oy, p.z - oz},
                          Velocity {v.x - dvx, v.y - dvy, v.z - dvz},
                          Mass {m},
                          Accel {0, 0, 0});
            }
        },
        times {1});
    s.run(world); // one flush places every body in its archetype
}

void build_schedule(Schedule& schedule) {
    // gather: copy every body into the flat array the force kernel scans.
    // Extract gives each work item a disjoint span at its own row offset, so
    // the array is filled in parallel with no merge -- the executor pre-sizes
    // it once in the barrier hook before the dispatch.
    schedule.add_parallel("gather",
                          [](Query<Position const, Mass const> q,
                             Extract<BodyArray> out) {
                              q.for_each_chunk([&](std::span<Entity const>,
                                                   chunk<Position const> p,
                                                   chunk<Mass const> mass) {
                                  auto const x  = p.column<0>();
                                  auto const y  = p.column<1>();
                                  auto const z  = p.column<2>();
                                  auto const kg = mass.column<0>();
                                  for (std::size_t i = 0; i < x.size(); ++i)
                                      out[i] = Body {x[i], y[i], z[i], kg[i]};
                              });
                          });

    // forces: the O(n^2) heart. Each row sums over the whole body array -- rows
    // are independent, so this is exactly the shape add_parallel slices, and it
    // is where essentially the entire tick is spent.
    //
    // Every lane walks the array in the SAME order for its own rows, so the
    // float sum per body is order-identical at any lane count -- the run is
    // bitwise reproducible without doing anything special (main() asserts it).
    schedule.add_parallel("forces",
                          [](Query<Position const, Mass const, Accel> q,
                             Res<BodyArray> bodies,
                             Reduce<Potential, AddPotential> pot) {
                              auto const& b       = *bodies;
                              std::size_t const n = b.size();
                              double u_local      = 0;
                              q.for_each([&](auto& p, auto& mass, auto& a) {
                                  float const xi = p.x, yi = p.y, zi = p.z;
                                  float ax = 0, ay = 0, az = 0;
                                  float pe = 0;
                                  // Branch-free: the j == i term has d = 0, so it adds
                                  // nothing to the acceleration. Softening is what makes
                                  // that safe.
                                  for (std::size_t j = 0; j < n; ++j) {
                                      float const dx = b[j].x - xi;
                                      float const dy = b[j].y - yi;
                                      float const dz = b[j].z - zi;
                                      float const r2 =
                                          dx * dx + dy * dy + dz * dz + cfg::kSoft2;
                                      float const inv = 1.0f / std::sqrt(r2);
                                      float const s = cfg::kG * b[j].m * inv * inv * inv;
                                      ax += dx * s;
                                      ay += dy * s;
                                      az += dz * s;
                                      pe -= cfg::kG * mass.kg * b[j].m * inv;
                                  }
                                  // The self term contributes 0 to the force but -G
                                  // m^2/eps to the potential; add it back rather than
                                  // branching in the loop.
                                  pe += cfg::kG * mass.kg * mass.kg / cfg::kSoft;
                                  a.x = ax;
                                  a.y = ay;
                                  a.z = az;
                                  u_local += 0.5 * double(pe); // every pair is seen twice
                              });
                              pot->u += u_local;
                          });

    // integrate: semi-implicit Euler -- kick then drift, using the acceleration
    // `forces` just wrote. Reading Accel is what levels this after forces.
    // Kinetic energy and momentum ride along for free while the velocity is hot.
    schedule.add_parallel("integrate",
                          [](Query<Position, Velocity, Mass const, Accel const> q,
                             Reduce<Motion, AddMotion> mot) {
                              Motion local;
                              q.for_each([&](auto& p, auto& v, auto& mass, auto& a) {
                                  v.x += a.x * cfg::kDt;
                                  v.y += a.y * cfg::kDt;
                                  v.z += a.z * cfg::kDt;
                                  p.x += v.x * cfg::kDt;
                                  p.y += v.y * cfg::kDt;
                                  p.z += v.z * cfg::kDt;
                                  double const m = mass.kg;
                                  local.ke += 0.5 * m *
                                              (double(v.x) * v.x + double(v.y) * v.y +
                                               double(v.z) * v.z);
                                  local.px += m * v.x;
                                  local.py += m * v.y;
                                  local.pz += m * v.z;
                              });
                              mot->ke += local.ke;
                              mot->px += local.px;
                              mot->py += local.py;
                              mot->pz += local.pz;
                          });

    // diagnostics: fold the two reductions into a conservation record. Ordered
    // side effects on one resource -- a serial system, leveled last by its reads
    // of what the two kernels produced.
    //
    // The energy is a half-step mixture: the potential comes from the positions
    // BEFORE this tick's drift, the kinetic from the velocities after the kick.
    // That is a constant offset for symplectic Euler, not a growing error, so
    // the DRIFT reported below is still the signal -- it just should not be read
    // as the exact total energy.
    schedule.add_serial(
        "diagnostics",
        [](Res<Potential> pot, Res<Motion> mot, ResMut<Diag> d) {
            d->e = mot->ke + pot->u;
            d->p = std::sqrt(mot->px * mot->px + mot->py * mot->py + mot->pz * mot->pz);
            if (!d->primed) {
                d->e0     = d->e;
                d->p0     = d->p;
                d->primed = true;
            }
            if (d->e0 != 0.0)
                d->max_drift = std::max(d->max_drift, std::abs((d->e - d->e0) / d->e0));
            d->max_p_drift = std::max(d->max_p_drift, std::abs(d->p - d->p0));
            ++d->ticks;
        });
}

struct RunResult {
    std::vector<float> state; // every body's position + velocity, serial order
    double ms_per_tick = 0;
    Diag diag {};
    std::size_t levels = 0;
};

RunResult run(unsigned const lanes, int const ticks, int const n) {
    World world;
    world.emplace_resource<BodyArray>();
    world.emplace_resource<Potential>();
    world.emplace_resource<Motion>();
    world.emplace_resource<Diag>();

    // Load, then loop: seed at load so every tick the schedule runs is a steady
    // tick, and prewarm can size the per-item state against a populated world.
    seed_bodies(world, n);

    Schedule s;
    build_schedule(s);

    WorkerPool pool {lanes};
    s.prewarm(world);
    for (int i = 0, wu = env_int("ECS_WARMUP", 0); i < wu; ++i)
        s.run(world, pool);

    auto const t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < ticks; ++t)
        s.run(world, pool);
    auto const t1 = std::chrono::steady_clock::now();

    RunResult r;
    r.levels      = s.level_count();
    r.ms_per_tick = std::chrono::duration<double, std::milli>(t1 - t0).count() / ticks;
    r.diag        = world.resource<Diag>();
    world.for_each<Position const, Velocity const>([&](auto& p, auto& v) {
        r.state.insert(r.state.end(), {p.x, p.y, p.z, v.x, v.y, v.z});
    });
    return r;
}

} // namespace

int main() {
    bench::set_suite("nbody");
    int const n     = env_int("ECS_BODIES", 4096);
    int const ticks = env_int("ECS_TICKS", 50);

    std::printf("naive O(n^2) n-body: %d bodies, %d ticks, dt=%.3f, eps=%.3f\n",
                n,
                ticks,
                double(cfg::kDt),
                double(cfg::kSoft));
    std::printf("%lld interactions per tick\n", (long long)n * n);

    auto const lanes = bench::lane_set();

    // How many work items the executor will cut `forces` into, by the same rule
    // WavePlan::build_parallel uses. It matters here in a way it does not for a
    // typical kernel: the grain floor exists to amortize dispatch over cheap
    // per-row work, but this kernel's per-row work is O(n), so below
    // kMinItemRows * lanes bodies there are simply fewer items than lanes and
    // the spare lanes have nothing to claim. Reported so a flat speedup reads
    // as "grain-limited" rather than "the executor does not scale" -- at 1024
    // bodies there is exactly ONE item and every lane count ties.
    constexpr std::size_t kMinItemRows = 1024; // WavePlan::kMinItemRows
    constexpr std::size_t kTargetItems = 64;   // WavePlan::kTargetItemsPerSystem
    std::size_t const grain = std::max(kMinItemRows, std::size_t(n) / kTargetItems);
    std::size_t const items = (std::size_t(n) + grain - 1) / grain;
    std::printf("work items for `forces`: %zu (grain %zu rows)\n", items, grain);
    if (items < lanes.back())
        std::printf("NOTE: %zu items < %u lanes -- speedup is capped at %zux by the "
                    "grain floor, not by the kernel. Use ECS_BODIES >= %zu to "
                    "saturate %u lanes.\n",
                    items,
                    lanes.back(),
                    items,
                    kMinItemRows * lanes.back(),
                    lanes.back());
    std::printf("\n");
    std::vector<RunResult> results;
    results.reserve(lanes.size());

    for (unsigned const l : lanes) {
        auto r = run(l, ticks, n);
        std::printf("%2u lane%s: %8.3f ms/tick  %6.1f Minteractions/s  "
                    "energy drift %.3e  |P| %.3e\n",
                    l,
                    l == 1 ? " " : "s",
                    r.ms_per_tick,
                    double(n) * double(n) / (r.ms_per_tick * 1e3),
                    r.diag.max_drift,
                    r.diag.p);
        bench::emit("naive",
                    "ms_per_tick",
                    r.ms_per_tick,
                    "ms",
                    "lower",
                    std::size_t(n),
                    l,
                    std::size_t(ticks));
        bench::emit("naive",
                    "energy_drift",
                    r.diag.max_drift,
                    "rel",
                    "lower",
                    std::size_t(n),
                    l,
                    std::size_t(ticks));
        results.push_back(std::move(r));
    }

    auto const& base = results.front();
    std::printf("\nschedule: %zu waves (gather -> forces -> integrate -> diagnostics)\n",
                base.levels);
    std::printf("energy: E0 = %.6f, E = %.6f, max |dE/E0| = %.3e over %llu ticks\n",
                base.diag.e0,
                base.diag.e,
                base.diag.max_drift,
                (unsigned long long)base.diag.ticks);
    std::printf("momentum: |P0| = %.3e, |P| = %.3e, max drift %.3e\n",
                base.diag.p0,
                base.diag.p,
                base.diag.max_p_drift);

    // Lane-count invariance: the library's headline determinism claim. Every
    // body's sum walks the same array in the same order regardless of how the
    // rows were sliced, so the result must be bitwise identical -- not merely
    // close.
    bool all_identical = true;
    for (std::size_t i = 1; i < results.size(); ++i) {
        bool const same = results[i].state == base.state;
        all_identical   = all_identical && same;
        if (lanes.size() > 1)
            std::printf("state %u-lane == %u-lane (bitwise): %s\n",
                        lanes[i],
                        lanes.front(),
                        same ? "YES" : "NO");
    }
    if (results.size() > 1) {
        double const speedup = base.ms_per_tick / results.back().ms_per_tick;
        std::printf("speedup %u -> %u lanes: %.2fx\n",
                    lanes.front(),
                    lanes.back(),
                    speedup);
        bench::emit("naive",
                    "speedup",
                    speedup,
                    "x",
                    "higher",
                    std::size_t(n),
                    lanes.back(),
                    std::size_t(ticks));
    }

    // A reference that is silently wrong is worse than none: fail loudly.
    bool ok = true;
    if (!all_identical) {
        std::printf("FAIL: results differ across lane counts\n");
        ok = false;
    }
    if (!(base.diag.max_drift < 1e-3)) {
        std::printf("FAIL: energy drift %.3e exceeds 1e-3\n", base.diag.max_drift);
        ok = false;
    }
    // Momentum is conserved exactly by a pairwise-symmetric force, so this
    // checks float rounding, not physics -- it catches an asymmetric or
    // double-counted interaction, which is precisely the bug a tree code is
    // prone to.
    if (!(base.diag.max_p_drift < 1e-5)) {
        std::printf("FAIL: momentum drift %.3e exceeds 1e-5\n", base.diag.max_p_drift);
        ok = false;
    }
    std::printf("%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
