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
// The three turbulence fields are independent and CPU-heavy, so they are the
// part a thread pool can actually speed up (see benchmarks/schedule_bench).
// Commands recorded by emitter/reaper flush at each wave barrier.
#include "simulation.hpp"

#include "ecs/ecs.hpp"
#include "particles.hpp"
#include <cmath>
#include <random>
#include <span>

using namespace ecs;

namespace {

// Divergence-free curl of a sin/cos potential, summed over kTurbOctaves octaves.
// Deliberately compute-bound (a few transcendentals per octave) -- this per-
// particle cost is the workload that makes parallel scheduling worthwhile.
inline void curl_force(float x, float y, float t, float seed, float& ox, float& oy) {
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
// field's per-particle force. Templated on the force component so swirl/drift/
// gust share the code while writing disjoint components (and so run in parallel).
template <class Force>
auto field_system(float seed) {
    return [seed](Query<Position const, Force> q, Res<Clock> clk) {
        float const t = clk->t;
        q.for_each_chunk([t, seed](std::span<Entity>,
                                   soa_storage<Position> const& pos,
                                   soa_storage<Force>& f) {
            auto px = pos.column<0>();
            auto py = pos.column<1>();
            auto fx = f.template column<0>(); // f is soa_storage<Force> (dependent)
            auto fy = f.template column<1>();
            for (std::size_t i = 0; i < px.size(); ++i)
                curl_force(px[i], py[i], t, seed, fx[i], fy[i]);
        });
    };
}

} // namespace

void build_particle_schedule(Schedule& schedule) {
    // clock: advance sim time (drives the animated fields). Tiny; wave 0.
    schedule.add("clock", [](ResMut<Clock> c) { c->t += cfg::kDt; });

    // emitter: spawn short-lived particles (with zeroed force fields) through
    // Commands, using the RNG resource for spread. wave 0.
    schedule.add("emitter", [](Commands& cmd, ResMut<Rng> rng) {
        auto& g = rng->gen;
        std::uniform_real_distribution<float> spreadX(-0.50f, 0.50f);
        std::uniform_real_distribution<float> launchY(1.20f, 2.60f);
        std::uniform_real_distribution<float> warmth(0.0f, 1.0f);
        std::uniform_real_distribution<float> lifespan(1.8f, 2.8f);
        for (int i = 0; i < cfg::kEmitPerTick; ++i) {
            float u = warmth(g); // deep orange -> bright yellow
            cmd.spawn(Position {cfg::kOriginX, cfg::kOriginY},
                      Velocity {spreadX(g), launchY(g)},
                      Color {1.0f, 0.50f + 0.35f * u, 0.10f + 0.15f * u},
                      Age {0.0f, lifespan(g)},
                      Swirl {0, 0},
                      Drift {0, 0},
                      Gust {0, 0});
        }
    });

    // gravity: read the Gravity resource, write Velocity. wave 0.
    schedule.add("gravity", [](Query<Velocity> q, Res<Gravity> grav) {
        float const a = grav->accel;
        q.for_each_chunk([a](std::span<Entity>, soa_storage<Velocity>& vel) {
            for (float& vy : vel.column<1>())
                vy += a * cfg::kDt;
        });
    });

    // age: advance each particle's clock. Writes Age, independent. wave 0.
    schedule.add("age", [](Query<Age> q) {
        q.for_each_chunk([](std::span<Entity>, soa_storage<Age>& age) {
            for (float& t : age.column<0>())
                t += cfg::kDt;
        });
    });

    // The three turbulence fields: independent + compute-bound, so the scheduler
    // runs them in parallel (wave 1). Different seeds -> multi-scale swirl.
    schedule.add("swirl", field_system<Swirl>(0.0f));
    schedule.add("drift", field_system<Drift>(1.7f));
    schedule.add("gust", field_system<Gust>(3.3f));

    // reaper: destroy particles past their lifespan (deferred). wave 1.
    schedule.add("reaper", [](Query<Age const> q, Commands& cmd) {
        q.each([&](Entity e, Age const& a) {
            if (a.t >= a.max)
                cmd.destroy(e);
        });
    });

    // accumulate: sum the three fields into Velocity. Reads what wave 1 wrote
    // and writes Velocity (which gravity wrote) -> wave 2.
    schedule.add("accumulate",
                 [](Query<Velocity, Swirl const, Drift const, Gust const> q) {
                     q.for_each_chunk([](std::span<Entity>,
                                         soa_storage<Velocity>& vel,
                                         soa_storage<Swirl> const& sw,
                                         soa_storage<Drift> const& dr,
                                         soa_storage<Gust> const& gu) {
                         auto vx       = vel.column<0>();
                         auto vy       = vel.column<1>();
                         auto sx       = sw.column<0>();
                         auto sy       = sw.column<1>();
                         auto dx       = dr.column<0>();
                         auto dy       = dr.column<1>();
                         auto gx       = gu.column<0>();
                         auto gy       = gu.column<1>();
                         float const s = cfg::kTurbStrength * cfg::kDt;
                         for (std::size_t i = 0; i < vx.size(); ++i) {
                             vx[i] += s * (sx[i] + dx[i] + gx[i]);
                             vy[i] += s * (sy[i] + dy[i] + gy[i]);
                         }
                     });
                 });

    // integrate: read the final Velocity, write Position. wave 3.
    schedule.add("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity>,
                            soa_storage<Position>& pos,
                            soa_storage<Velocity> const& vel) {
            auto px = pos.column<0>();
            auto py = pos.column<1>();
            auto vx = vel.column<0>();
            auto vy = vel.column<1>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] += vx[i] * cfg::kDt;
                py[i] += vy[i] * cfg::kDt;
            }
        });
    });

    // extract: publish a draw-ready snapshot of the survivors. wave 4.
    schedule.add("extract",
                 [](Query<Position const, Color const, Age const> q,
                    ResMut<TripleBuffer<RenderSnapshot>> ch) {
                     RenderSnapshot& out = ch->back();
                     out.clear();
                     q.each([&](Entity, Position const& p, Color const& c, Age const& a) {
                         float alpha = 1.0f - a.t / a.max; // linear fade-out
                         if (alpha < 0.0f)
                             alpha = 0.0f;
                         out.push_back(GpuParticle {p.x, p.y, c.r, c.g, c.b, alpha});
                     });
                     ch->publish();
                 });
}
