// examples/particles/simulation.cpp
//
// The particle fountain as a schedule of ECS systems. Each system's read/write
// access is derived from its parameter types and the scheduler levels them into
// waves automatically:
//
//   wave 0: clock | emitter | age | gravity     (independent)
//   wave 1: swirl | drift | gust | reaper        (the 3 force fields -- the
//                                                  compute-bound, parallel work)
//   wave 2: accumulate                           (sum the fields into Velocity)
//   wave 3: integrate                            (Velocity -> Position)
//   wave 4: extract                              (publish the draw-ready frame)
//
// The per-entity systems are registered with `add_parallel`, so the executor
// splits each system's row range across the pool's lanes (data parallelism
// within a system); each work item then iterates its slice with `for_each_chunk`
// -- the SoA access shape, not the source of the parallelism. The few
// structural/gather systems (clock, emitter, reaper, extract) stay imperative
// `add` and iterate serially. Commands flush at each wave barrier.
#include "simulation.hpp"

#include <ecs/ecs.hpp>

#include "particles.hpp"
#include <cmath>
#include <cstddef>
#include <random>
#include <span>

using namespace ecs;

namespace {

// Divergence-free curl of a sin/cos potential, summed over kTurbOctaves octaves.
// Deliberately compute-bound (a few transcendentals per octave) -- this per-
// particle cost is the workload that makes parallel scheduling worthwhile.
void curl_force(float x, float y, float t, float seed, float& ox, float& oy) {
    float fx = 0.0f, fy = 0.0f;
    float freq = 1.3f + seed;
    float amp  = 1.0f;
    for (int o = 0; o < cfg::kTurbOctaves; ++o) {
        float const a = freq, b = freq * 1.7f;
        float const ph = t * (0.6f + 0.2f * float(o)) + seed * 3.1f;
        // potential psi = sin(a*x+ph)*cos(b*y-ph); force = (dpsi/dy, -dpsi/dx)
        float const sx = std::sin(a * x + ph), cx = std::cos(a * x + ph);
        float const sy = std::sin(b * y - ph), cy = std::cos(b * y - ph);
        fx += amp * -b * sx * sy;
        fy += amp * -a * cx * cy;
        freq *= 1.9f;
        amp *= 0.55f;
    }
    ox = fx;
    oy = fy;
}

// One turbulence field as a system: read positions + the clock, write this
// field's per-particle force for the lane's slice of rows. Templated on the
// force component so swirl/drift/gust share the code while writing disjoint
// components.
template <class Force>
auto field_system(float seed) {
    return [seed](Query<Position const, Force> q, Res<Clock> clk) {
        float const t = clk->t;
        q.for_each_chunk([t, seed](std::span<Entity const>,
                                   chunk<Position const> pos,
                                   chunk<Force> f) {
            auto px = pos.column<0>();
            auto py = pos.column<1>();
            auto fx = f.template column<0>();
            auto fy = f.template column<1>();
            for (std::size_t i = 0; i < px.size(); ++i)
                curl_force(px[i], py[i], t, seed, fx[i], fy[i]);
        });
    };
}

} // namespace

void build_particle_schedule(Schedule& schedule) {
    // clock: advance sim time (drives the animated fields). Tiny; serial. wave 0.
    schedule.add_serial("clock", [](ResMut<Clock> c) { c->t += cfg::kDt; });

    // emitter: spawn short-lived particles (with zeroed force fields) through
    // Commands, reading the Emitter resource (rate + nozzle) and the RNG for
    // spread. Serial (structural). wave 0.
    schedule.add_serial("emitter", [](Commands& cmd, ResMut<Rng> rng, Res<Emitter> em) {
        auto& g = rng->gen;
        std::uniform_real_distribution<float> spreadX(-0.50f, 0.50f);
        std::uniform_real_distribution<float> launchY(1.20f, 2.60f);
        std::uniform_real_distribution<float> warmth(0.0f, 1.0f);
        std::uniform_real_distribution<float> lifespan(1.8f, 2.8f);
        for (int i = 0; i < em->per_tick; ++i) {
            float u = warmth(g); // deep orange -> bright yellow
            cmd.spawn(Position {em->origin_x, em->origin_y},
                      Velocity {spreadX(g), launchY(g)},
                      Color {1.0f, 0.50f + 0.35f * u, 0.10f + 0.15f * u},
                      Age {0.0f, lifespan(g)},
                      Swirl {0, 0},
                      Drift {0, 0},
                      Gust {0, 0});
        }
    });

    // gravity: read the Gravity resource, write Velocity (data-parallel). wave 0.
    schedule.add_parallel("gravity", [](Query<Velocity> q, Res<Gravity> grav) {
        float const a = grav->accel;
        q.for_each_chunk([a](std::span<Entity const>, chunk<Velocity> vel) {
            for (auto& vy : vel.column<1>())
                vy += a * cfg::kDt;
        });
    });

    // age: advance each particle's clock (data-parallel). wave 0.
    schedule.add_parallel("age", [](Query<Age> q) {
        q.for_each_chunk([](std::span<Entity const>, chunk<Age> age) {
            for (auto& t : age.column<0>())
                t += cfg::kDt;
        });
    });

    // The three turbulence fields: independent + compute-bound. Each is split
    // across lanes (within-system) *and* they are independent (across-system),
    // so the pool has plenty of parallel work. wave 1.
    schedule.add_parallel("swirl", field_system<Swirl>(0.0f));
    schedule.add_parallel("drift", field_system<Drift>(1.7f));
    schedule.add_parallel("gust", field_system<Gust>(3.3f));

    // reaper: destroy particles past their lifespan (deferred). Serial. wave 1.
    schedule.add_serial("reaper", [](Query<Age const> q, Commands& cmd) {
        q.for_each([&](Entity e, auto& a) {
            if (a.t >= a.max)
                cmd.destroy(e);
        });
    });

    // accumulate: sum the three fields into Velocity (data-parallel). wave 2.
    schedule.add_parallel("accumulate",
                          [](Query<Velocity, Swirl const, Drift const, Gust const> q) {
                              q.for_each_chunk([](std::span<Entity const>,
                                                  chunk<Velocity> vel,
                                                  chunk<Swirl const> sw,
                                                  chunk<Drift const> dr,
                                                  chunk<Gust const> gu) {
                                  auto vx       = vel.column<0>();
                                  auto vy       = vel.column<1>();
                                  auto const sx = sw.column<0>();
                                  auto const sy = sw.column<1>();
                                  auto const dx = dr.column<0>();
                                  auto const dy = dr.column<1>();
                                  auto const gx = gu.column<0>();
                                  auto const gy = gu.column<1>();
                                  float const s = cfg::kTurbStrength * cfg::kDt;
                                  for (std::size_t i = 0; i < vx.size(); ++i) {
                                      vx[i] += s * (sx[i] + dx[i] + gx[i]);
                                      vy[i] += s * (sy[i] + dy[i] + gy[i]);
                                  }
                              });
                          });

    // integrate: read the final Velocity, write Position (data-parallel). wave 3.
    schedule.add_parallel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity const>,
                            chunk<Position> pos,
                            chunk<Velocity const> vel) {
            auto px       = pos.column<0>();
            auto py       = pos.column<1>();
            auto const vx = vel.column<0>();
            auto const vy = vel.column<1>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] += vx[i] * cfg::kDt;
                py[i] += vy[i] * cfg::kDt;
            }
        });
    });

    // extract: publish a draw-ready snapshot of the survivors. Serial (gather
    // into the shared TripleBuffer). wave 4.
    schedule.add_serial("extract",
                        [](Query<Position const, Color const, Age const> q,
                           ResMut<TripleBuffer<RenderSnapshot>> ch) {
                            RenderSnapshot& out = ch->back();
                            out.clear();
                            q.for_each([&](auto& p, auto& c, auto& a) {
                                float alpha = 1.0f - a.t / a.max; // linear fade-out
                                if (alpha < 0.0f)
                                    alpha = 0.0f;
                                out.push_back(
                                    GpuParticle {p.x, p.y, c.r, c.g, c.b, alpha});
                            });
                            ch->publish();
                        });
}
