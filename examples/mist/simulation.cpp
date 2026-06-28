// examples/mist/simulation.cpp
//
// A starling murmuration as a schedule of ECS systems. Access is derived from
// each system's parameter types and leveled into waves automatically:
//
//   phase -1: scatter                 (one-shot: seed the flock)
//   wave 0:   input | clock | goal | grid
//   wave 1:   steer                   (boids + goal + predator -> Velocity)
//   wave 2:   integrate               (Velocity -> Position)
//   wave 3:   extract                 (publish the draw-ready frame)
//
// `grid` reduces every bird into a FlockGrid resource (per-cell density + sums
// of position and velocity); `steer` (the compute-bound, for_each_chunk-parallel
// kernel) reads each bird's cell + 6 face neighbours to apply Reynolds boids --
// cohesion (toward the local centre of mass), alignment (toward the local mean
// heading) and separation (away from crowding) -- plus a pull toward the slowly
// wandering `goal` (which flies the whole flock around) and a shove away from
// the cursor's screen ray (a predator). The accumulated steering is applied as
// an acceleration, then speed is pulled back to a cruise: steering turns the
// birds, it does not speed them up or stop them, which is what makes the flock
// read as *flight* rather than drifting. Commands flush at each wave barrier.
#include "simulation.hpp"

#include "ecs/ecs.hpp"
#include "mist.hpp"
#include <cmath>
#include <cstddef>
#include <random>
#include <span>

using namespace ecs;

namespace {

// Add `w` units of the unit vector along (dx,dy,dz) into the accumulator. A
// zero-length input contributes nothing. This is the Reynolds "steer toward a
// direction" primitive each rule uses.
inline void add_dir(float& ax, float& ay, float& az,
                    float dx, float dy, float dz, float w) {
    float const l2 = dx * dx + dy * dy + dz * dz;
    if (l2 > 1e-12f) {
        float const s = w / std::sqrt(l2);
        ax += dx * s;
        ay += dy * s;
        az += dz * s;
    }
}

} // namespace

void build_mist_schedule(Schedule& schedule, MistInput in, int count) {
    // scatter: seed the flock once in a loose blob with small random velocities,
    // then remove itself. phase<-1> runs before the per-tick work.
    schedule.add_once(
        "scatter",
        [count](Commands& cmd, ResMut<Rng> rng) {
            auto& g = rng->gen;
            std::uniform_real_distribution<float> p(-0.8f, 0.8f);
            std::uniform_real_distribution<float> v(-0.1f, 0.1f);
            for (int i = 0; i < count; ++i)
                cmd.spawn(Position {p(g), p(g), p(g) - 0.3f},
                          Velocity {v(g), v(g), v(g)});
        },
        phase<-1> {});

    // input: copy the latest cursor (and window aspect) into the Cursor
    // resource on the sim thread (single-writer); `steer` reads it. wave 0.
    schedule.add("input", [in](ResMut<Cursor> cur) {
        std::uint64_t const c = in.cursor->load(std::memory_order_relaxed);
        std::uint32_t const f = in.flags->load(std::memory_order_relaxed);
        std::uint64_t const s = in.fb_size->load(std::memory_order_relaxed);
        cur->x      = cursor_x(c);
        cur->y      = cursor_y(c);
        cur->active = (f & 1u) != 0;
        int const w = int(s >> 32), h = int(s & 0xffffffffu);
        cur->aspect = h ? float(w) / float(h) : 1.0f;
    });

    // clock: advance sim time (drives the goal's wander). wave 0.
    schedule.add("clock", [](ResMut<Clock> c) { c->t += cfg::kDt; });

    // goal: move the global attractor on a slow Lissajous through the volume.
    // Its z swings up near the camera, so the flock periodically sweeps over the
    // viewer. wave 0.
    schedule.add("goal", [](ResMut<Goal> go, Res<Clock> clk) {
        float const t = clk->t;
        // Wander in screen space (sx, sy) at a swinging depth gz, then convert
        // to a world position that projects to (sx, sy) -- so the flock stays in
        // view at every depth, and looms centred as gz nears the camera.
        float const sx = cfg::kRoamX * std::sin(0.062f * t) * std::cos(0.035f * t + 0.5f);
        float const sy = cfg::kRoamY * std::sin(0.051f * t + 1.3f);
        float const gz = cfg::kRoamZc + cfg::kRoamZ * std::sin(0.043f * t + 0.4f);
        float const d  = cfg::kCamZ - gz;
        go->x = sx * d / cfg::kFocal;
        go->y = sy * d / cfg::kFocal;
        go->z = gz;
    });

    // grid: reduce every bird into the spatial grid (per-cell count + position
    // and velocity sums). Reads Position + Velocity, writes the FlockGrid
    // resource. Serial reduction (`each`). wave 0.
    schedule.add("grid", [](Query<Position const, Velocity const> q, ResMut<FlockGrid> g) {
        g->clear();
        q.each([&](Entity, Position const& p, Velocity const& v) {
            int const c = FlockGrid::index(FlockGrid::axis(p.x),
                                           FlockGrid::axis(p.y),
                                           FlockGrid::axis(p.z));
            g->count[c] += 1;
            g->sx[c] += p.x; g->sy[c] += p.y; g->sz[c] += p.z;
            g->vx[c] += v.x; g->vy[c] += v.y; g->vz[c] += v.z;
        });
    });

    // steer: turn each bird. Accumulate the boids rules + goal + predator into a
    // steering acceleration, apply it, then pull speed back to the cruise.
    schedule.add("steer",
                 [](Query<Position const, Velocity> q,
                    Res<FlockGrid> grid,
                    Res<Goal> goal,
                    Res<Cursor> cur,
                    Res<Clock> clk) {
        Goal const G       = *goal;
        Cursor const C     = *cur;
        float const t      = clk->t;
        FlockGrid const& g = *grid;
        q.for_each_chunk([G, C, t, &g](std::span<Entity>,
                                    chunk<Position const> pos,
                                    chunk<Velocity> vel) {
            auto const px = pos.column<0>();
            auto const py = pos.column<1>();
            auto const pz = pos.column<2>();
            auto vx       = vel.column<0>();
            auto vy       = vel.column<1>();
            auto vz       = vel.column<2>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                float const x = px[i], y = py[i], z = pz[i];
                float ax = 0, ay = 0, az = 0; // steering acceleration

                // Local neighbourhood: cell + 6 face neighbours.
                int const ix = FlockGrid::axis(x);
                int const iy = FlockGrid::axis(y);
                int const iz = FlockGrid::axis(z);
                int const c  = FlockGrid::index(ix, iy, iz);
                int nn = 0;
                float spx = 0, spy = 0, spz = 0, svx = 0, svy = 0, svz = 0;
                int const nb[7] = {c,
                                   g.widx(ix - 1, iy, iz), g.widx(ix + 1, iy, iz),
                                   g.widx(ix, iy - 1, iz), g.widx(ix, iy + 1, iz),
                                   g.widx(ix, iy, iz - 1), g.widx(ix, iy, iz + 1)};
                for (int k : nb) {
                    nn += g.count[k];
                    spx += g.sx[k]; spy += g.sy[k]; spz += g.sz[k];
                    svx += g.vx[k]; svy += g.vy[k]; svz += g.vz[k];
                }
                float const invn = 1.0f / float(nn); // nn >= 1 (self counted)

                // Cohesion: toward the local centre of mass.
                add_dir(ax, ay, az, spx * invn - x, spy * invn - y, spz * invn - z,
                        cfg::kCohesion);
                // Alignment: toward the local mean heading.
                add_dir(ax, ay, az, svx, svy, svz, cfg::kAlignment);

                // Separation: only crowded cells push down the density gradient.
                if (float(g.count[c]) > cfg::kSoftCap) {
                    float const sgx = float(g.count_at(ix - 1, iy, iz) - g.count_at(ix + 1, iy, iz));
                    float const sgy = float(g.count_at(ix, iy - 1, iz) - g.count_at(ix, iy + 1, iz));
                    float const sgz = float(g.count_at(ix, iy, iz - 1) - g.count_at(ix, iy, iz + 1));
                    add_dir(ax, ay, az, sgx, sgy, sgz, cfg::kSeparation);
                }

                // Goal seek: the flock always flies toward its wandering roost.
                add_dir(ax, ay, az, G.x - x, G.y - y, G.z - z, cfg::kGoalSeek);

                // Cursor lure (local): a bird whose screen position falls within
                // kCursorRadius of the pointer is gently drawn toward it, with a
                // falloff -- you tease a tendril of the flock to the cursor while
                // the rest keeps flying its pattern.
                float const d = cfg::kCamZ - z;
                if (C.active && d > cfg::kNearClip) {
                    float const inv = cfg::kFocal / d;
                    float const sdx = C.x - x * inv;
                    float const sdy = C.y - y * inv;
                    float const sd  = std::sqrt(sdx * sdx + sdy * sdy);
                    if (sd < cfg::kCursorRadius) {
                        float fall = 1.0f - sd / cfg::kCursorRadius;
                        fall *= fall;
                        add_dir(ax, ay, az, sdx, sdy, 0.0f, cfg::kAttract * fall);
                    }
                }

                // Wander: per-bird 3D turbulence -- neighbours diverge a little,
                // which breaks thin lines/sheets into volume and adds a living
                // shimmer.
                float const k = cfg::kWanderFreq;
                add_dir(ax, ay, az,
                        std::sin(k * y + 0.7f * t) + std::sin(1.7f * k * z - 0.5f * t),
                        std::sin(k * z + 0.6f * t) + std::sin(1.7f * k * x + 0.4f * t),
                        std::sin(k * x - 0.8f * t) + std::sin(1.7f * k * y + 0.5f * t),
                        cfg::kWander);

                // Soft boundary: steer back when straying past the roam radius.
                float const r2 = x * x + y * y + z * z;
                if (r2 > cfg::kBound * cfg::kBound) {
                    float const r = std::sqrt(r2);
                    add_dir(ax, ay, az, -x, -y, -z, cfg::kBoundForce * (r - cfg::kBound));
                }

                // Apply the steering, then pull speed back to the cruise so the
                // birds always fly (turning, not speeding up or stopping).
                float nvx = vx[i] + ax * cfg::kDt;
                float nvy = vy[i] + ay * cfg::kDt;
                float nvz = vz[i] + az * cfg::kDt;
                float const sp = std::sqrt(nvx * nvx + nvy * nvy + nvz * nvz);
                if (sp > 1e-5f) {
                    float const f = cfg::kCruise / sp;
                    nvx *= f; nvy *= f; nvz *= f;
                }
                vx[i] = nvx; vy[i] = nvy; vz[i] = nvz;
            }
        });
    });

    // integrate: advance Position by Velocity. No wrap -- the flock flies free,
    // held in the roaming region by steer's soft boundary. wave 2.
    schedule.add("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity>,
                            chunk<Position> pos,
                            chunk<Velocity const> vel) {
            auto px       = pos.column<0>();
            auto py       = pos.column<1>();
            auto pz       = pos.column<2>();
            auto const vx = vel.column<0>();
            auto const vy = vel.column<1>();
            auto const vz = vel.column<2>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] += vx[i] * cfg::kDt;
                py[i] += vy[i] * cfg::kDt;
                pz[i] += vz[i] * cfg::kDt;
            }
        });
    });

    // extract: publish a draw-ready snapshot of every bird's position (the
    // shader culls/fades by depth). Serial gather into the TripleBuffer. wave 3.
    schedule.add("extract",
                 [](Query<Position const> q, ResMut<TripleBuffer<RenderSnapshot>> ch) {
                     RenderSnapshot& out = ch->back();
                     out.clear();
                     q.each([&](Entity, Position const& p) {
                         out.push_back(GpuParticle {p.x, p.y, p.z});
                     });
                     ch->publish();
                 });
}
