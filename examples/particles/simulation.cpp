// examples/particles/simulation.cpp
//
// The particle fountain as a schedule of ECS systems. Each system's read/write
// access is derived from its parameter types, and the scheduler levels them
// automatically into three waves:
//
//   wave 0: emitter | gravity | age      (independent writes -> parallel)
//   wave 1: integrate | reaper           (read what wave 0 wrote)
//   wave 2: extract                       (reads Position from integrate)
//
// Commands recorded by emitter/reaper flush at each wave barrier, so a particle
// spawned this tick is integrated this tick, and a reaped one is gone before
// extract publishes.
#include "simulation.hpp"

#include "ecs/ecs.hpp"
#include "particles.hpp"
#include <random>
#include <span>

using namespace ecs;

void build_particle_schedule(Schedule& schedule) {
    // emitter: spawn short-lived particles through Commands (deferred), using
    // the RNG resource for spread. Commands carries no tracked access, so this
    // shares wave 0 with gravity and age.
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
                      Age {0.0f, lifespan(g)});
        }
    });

    // gravity: read the Gravity resource, write Velocity. SoA fast path on the
    // velocity-y column.
    schedule.add("gravity", [](Query<Velocity> q, Res<Gravity> grav) {
        float const a = grav->accel;
        q.for_each_chunk([a](std::span<Entity>, soa_storage<Velocity>& vel) {
            for (float& vy : vel.column<1>())
                vy += a * cfg::kDt;
        });
    });

    // age: advance each particle's clock. Writes Age, independent of gravity.
    schedule.add("age", [](Query<Age> q) {
        q.for_each_chunk([](std::span<Entity>, soa_storage<Age>& age) {
            for (float& t : age.column<0>())
                t += cfg::kDt;
        });
    });

    // integrate: read Velocity (written by gravity -> later wave), write
    // Position. The hot loop: contiguous per-field columns, no gather/scatter.
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

    // reaper: destroy particles past their lifespan (deferred via Commands).
    schedule.add("reaper", [](Query<Age const> q, Commands& cmd) {
        q.each([&](Entity e, Age const& a) {
            if (a.t >= a.max)
                cmd.destroy(e);
        });
    });

    // extract: read the surviving particles and publish a draw-ready snapshot.
    // Runs last (reads Position written by integrate); the reaper's destroys
    // have already flushed, so no dead particles are emitted.
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
