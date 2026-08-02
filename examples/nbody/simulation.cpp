// examples/nbody/simulation.cpp
//
// A real O(n^2) gravitational n-body simulation -- every body pulled by every
// other, no approximation. It exists to be the REFERENCE the Barnes-Hut demo is
// measured against, for accuracy and for speed, so it stays deliberately simple
// and obviously correct rather than clever.
//
// The interesting part for the library is how PAIRWISE work is expressed. A
// system takes at most one Query, and a parallel system's Query is sliced into
// per-item row ranges -- so "every body reads every other body" cannot be
// written as a query at all. The shape the add_parallel static_assert
// prescribes (schedule/schedule.hpp) is to build a structure and read it
// through a resource:
//
//   gather   Extract<BodyArray>  -- disjoint per-item spans write a flat
//                                   {x,y,z,m} array straight into the resource
//   forces   Res<BodyArray>      -- each row sums over the whole array
//
// Nothing declares an order. Access alone levels the four systems:
//
//   gather      reads Position, Mass          writes BodyArray
//   forces      reads Position, Mass, Body..  writes Accel, Potential
//   integrate   reads Accel, Mass             writes Position, Velocity, Motion
//   diagnostics reads Potential, Motion       writes Diag
//
// Physics: Plummer-softened gravity, a_i = sum_j G m_j d_ij / (|d|^2+eps^2)^1.5,
// advanced by semi-implicit (symplectic) Euler -- v += a dt, then x += v dt.
// Symplectic matters for a reference: energy error stays bounded and
// oscillatory rather than drifting monotonically, so "is the integrator or the
// force wrong?" has a usable answer. Softening is what keeps the inner loop
// branch-free -- a close pair is bounded rather than special-cased.
#include "simulation.hpp"

#include <ecs/ecs.hpp>

#include "nbody.hpp"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <span>
#include <vector>

using namespace ecs;

namespace {

// Fold operators for the two reductions. Implementation detail of how the
// schedule accumulates, so they live here rather than beside the data.
struct AddPotential {
    void operator()(Potential& a, Potential& b) const { a.u += b.u; }
};

struct AddMotion {
    void operator()(Motion& a, Motion& b) const {
        a.ke += b.ke;
        a.px += b.px;
        a.py += b.py;
        a.pz += b.pz;
    }
};

} // namespace

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
// of order v_rms / sqrt(n) -- small, conserved, and entirely spurious: the whole
// system would coast off in that direction forever. Centring the positions
// likewise puts the centre of mass at the origin. Standard n-body housekeeping,
// and it makes |P| ~ 0 an invariant worth asserting instead of a random number
// that merely stays constant.
void seed_bodies(World& world, int const n) {
    world.emplace_resource<BodyArray>();
    world.emplace_resource<Potential>();
    world.emplace_resource<Motion>();
    world.emplace_resource<Diag>();

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

void build_nbody_schedule(Schedule& schedule) {
    // gather: copy every body into the flat array the force kernel scans.
    // Extract gives each work item a disjoint span at its own row offset, so the
    // array is filled in parallel with no merge -- the executor pre-sizes it
    // once in the barrier hook before the dispatch.
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
                                  // Branch-free: the j == i term has d = 0, so
                                  // it adds nothing to the acceleration.
                                  // Softening is what makes that safe.
                                  for (std::size_t j = 0; j < n; ++j) {
                                      float const dx = b[j].x - xi;
                                      float const dy = b[j].y - yi;
                                      float const dz = b[j].z - zi;
                                      float const r2 =
                                          dx * dx + dy * dy + dz * dz + cfg::kSoft2;
                                      float const invr = 1.0f / std::sqrt(r2);
                                      float const s =
                                          cfg::kG * b[j].m * invr * invr * invr;
                                      ax += dx * s;
                                      ay += dy * s;
                                      az += dz * s;
                                      pe -= cfg::kG * mass.kg * b[j].m * invr;
                                  }
                                  // The self term contributes 0 to the force but
                                  // -G m^2/eps to the potential; add it back
                                  // rather than branching in the loop.
                                  pe += cfg::kG * mass.kg * mass.kg / cfg::kSoft;
                                  a.x = ax;
                                  a.y = ay;
                                  a.z = az;
                                  u_local += 0.5 * double(pe); // pairs seen twice
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
    // For symplectic Euler that is a constant offset, not a growing error, so
    // the DRIFT below is still the signal -- it just should not be read as the
    // exact total energy.
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
