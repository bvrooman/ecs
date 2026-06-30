// examples/mist/simulation.cpp
//
// A starling murmuration as a schedule of ECS systems. Access is derived from
// each system's parameter types and leveled into waves automatically:
//
//   phase -1: scatter                 (one-shot: seed the flock)
//   wave 0:   input | clock | goal | grid
//   wave 1:   steer                   (boids + goal -> Velocity)
//   wave 2:   integrate               (Velocity -> Position)
//   wave 3:   extract                 (publish the draw-ready frame)
//
// `grid` reduces every bird into a FlockGrid resource (per-cell density + sums
// of position and velocity); `steer` (the compute-bound, for_each_chunk-parallel
// kernel) reads each bird's cell + 6 face neighbours to apply Reynolds boids --
// cohesion (toward the local centre of mass), alignment (toward the local mean
// heading) and separation (away from crowding) -- plus a pull toward the slowly
// wandering `goal` (which flies the whole flock around, and which the cursor
// takes over when you steer). The accumulated steering is applied as
// an acceleration, then speed is pulled back to a cruise: steering turns the
// birds, it does not speed them up or stop them, which is what makes the flock
// read as *flight* rather than drifting. Commands flush at each wave barrier.
#include "simulation.hpp"

#include "ecs/ecs.hpp"
#include "mist.hpp"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <span>

using namespace ecs;

namespace {

// Add `w` units of the unit vector along (dx,dy,dz) into the accumulator. A
// zero-length input contributes nothing. This is the Reynolds "steer toward a
// direction" primitive each rule uses.
inline void add_dir(
    float& ax, float& ay, float& az, float dx, float dy, float dz, float w) {
    float const l2 = dx * dx + dy * dy + dz * dz;
    if (l2 > 1e-12f) {
        float const s = w / std::sqrt(l2);
        ax += dx * s;
        ay += dy * s;
        az += dz * s;
    }
}

// Runtime-overridable steering weights, so the flight can be swept/tuned without
// recompiling (e.g. ECS_GOALSEEK=1.6 ECS_SOFTCAP=60 ./mist). Defaults are the
// cfg values; the grid and camera stay compile-time.
struct Tuning {
    float cruise, goalSeek, goalFollow, cohesion, alignment, separation, softCap, wander;
    float dive; // fly-over dive depth
};
inline float env_f(char const* key, float dflt) {
    char const* v = std::getenv(key);
    return (v && *v) ? float(std::atof(v)) : dflt;
}
inline Tuning read_tuning() {
    return Tuning {env_f("ECS_CRUISE", cfg::kCruise),
                   env_f("ECS_GOALSEEK", cfg::kGoalSeek),
                   env_f("ECS_GOALFOLLOW", cfg::kGoalFollow),
                   env_f("ECS_COHESION", cfg::kCohesion),
                   env_f("ECS_ALIGN", cfg::kAlignment),
                   env_f("ECS_SEPARATION", cfg::kSeparation),
                   env_f("ECS_SOFTCAP", cfg::kSoftCap),
                   env_f("ECS_WANDER", cfg::kWander),
                   env_f("ECS_DIVE", cfg::kDiveAmp)};
}

} // namespace

void build_mist_schedule(Schedule& schedule, MistInput in, int count) {
    Tuning const T = read_tuning(); // steering weights (env-overridable, for sweeps)
    // scatter: seed the flock once in a loose blob with small random velocities,
    // then remove itself. phase<-1> runs before the per-tick work.
    schedule.add_once(
        "scatter",
        [count](Commands& cmd, ResMut<Rng> rng) {
            auto& g = rng->gen;
            std::uniform_real_distribution<float> p(-0.45f, 0.45f);
            std::uniform_real_distribution<float> j(-0.06f, 0.06f);
            // Start as a gently swirling disc centred on screen, at the working
            // depth (~kRoamZc). The velocity is a curl (rotational) field, v ~
            // (-y, x): locally coherent -- neighbours share a heading, so
            // alignment stays happy and it reads as a murmuration from t=0 -- but
            // its centre of mass stays put. A single shared heading instead
            // translated the whole flock off-screen (it rushed to the top-edge
            // before the goal could rein it back); the swirl lingers in the middle.
            for (int i = 0; i < count; ++i) {
                float const px = p(g), py = p(g);
                // Swirl (-y, x) + a gentle shared forward drift (+z): the drift
                // gives the dead-centre birds (whose swirl velocity is ~0) a
                // coherent heading too, so the centre doesn't evacuate into a ring.
                cmd.spawn(Position {px, py, cfg::kRoamZc + p(g)},
                          Velocity {-py + j(g), px + j(g), 0.20f + j(g)});
            }
        },
        phase<-1> {});

    // input: copy the latest cursor into the Cursor resource on the sim thread
    // (single-writer); `goal`/`steer` read it. wave 0.
    schedule.add("input", [in](ResMut<Cursor> cur) {
        std::uint64_t const c = in.cursor->load(std::memory_order_relaxed);
        std::uint32_t const f = in.flags->load(std::memory_order_relaxed);
        cur->x      = cursor_x(c);
        cur->y      = cursor_y(c);
        cur->active = (f & 1u) != 0;
    });

    // clock: advance sim time (drives the goal's wander). wave 0.
    schedule.add("clock", [](ResMut<Clock> c) { c->t += cfg::kDt; });

    // goal: set the global attractor the whole flock flies toward. This is the
    // single force that steers the murmuration as a mass, so the *cursor steers
    // here*, not as a local per-bird pull (which only ever made a swarm point).
    // When the cursor is active it *is* the goal -- point where the flock should
    // fly and the whole mass turns and heads there. Otherwise the goal wanders on
    // a slow Lissajous on its own. The xy target is in screen space (so the flock
    // stays in view at any depth) and converted to world at the goal's depth; the
    // depth gz keeps its autonomous swing either way, so the overhead sweeps
    // persist whether or not you are steering. wave 0.
    schedule.add("goal", [T](ResMut<Goal> go, Res<Clock> clk, Res<Cursor> cur) {
        float const t = clk->t;
        float sx, sy;
        if (cur->active) {
            sx = cur->x; // the cursor is the destination -- you fly the flock
            sy = cur->y;
        } else {
            sx = cfg::kRoamX * std::sin(0.062f * t) * std::cos(0.035f * t + 0.5f);
            sy = cfg::kRoamY *
                 std::sin(0.051f * t); // phase 0: starts centred, not at the top edge
        }
        // Depth: the gentle autonomous swing, plus a periodic fly-over dive --
        // a sharp pulse toward the camera (pow() is ~0 most of the cycle, then a
        // brief spike). Clamped just in front of the cull plane so the screen-space
        // projection stays valid (d>0) while the flock's leading edge streams past.
        // Only hands-off: while you steer, the dive is suppressed so the follow
        // stays smooth (a surprise surge cameraward would fight your control).
        float const dive =
            cur->active
                ? 0.0f
                : T.dive * std::pow(std::max(0.0f, std::sin(cfg::kDiveFreq * t)), 8.0f);
        float gz      = cfg::kRoamZc + cfg::kRoamZ * std::sin(0.043f * t + 0.4f) + dive;
        gz            = std::min(gz, 1.35f);
        float const d = cfg::kCamZ - gz;
        go->x         = sx * d / cfg::kFocal;
        go->y         = sy * d / cfg::kFocal;
        go->z         = gz;
    });

    // grid: reduce every bird into the spatial grid (per-cell count + position
    // and velocity sums). Reads Position + Velocity, writes the FlockGrid
    // resource. Serial reduction (`each`). wave 0.
    schedule.add("grid",
                 [](Query<Position const, Velocity const> q, ResMut<FlockGrid> g) {
                     g->clear();
                     q.each([&](Entity, Position const& p, Velocity const& v) {
                         int const c = FlockGrid::index(FlockGrid::axis(p.x),
                                                        FlockGrid::axis(p.y),
                                                        FlockGrid::axis(p.z));
                         g->count[c] += 1;
                         g->sx[c] += p.x;
                         g->sy[c] += p.y;
                         g->sz[c] += p.z;
                         g->vx[c] += v.x;
                         g->vy[c] += v.y;
                         g->vz[c] += v.z;
                     });
                 });

    // steer: turn each bird. Accumulate the boids rules + goal into a steering
    // acceleration, apply it, then pull speed back to the cruise. (The cursor
    // acts through the goal above, so there is no per-bird cursor force here.)
    schedule.add("steer",
                 [T](Query<Position const, Velocity> q,
                     Res<FlockGrid> grid,
                     Res<Goal> goal,
                     Res<Cursor> cur,
                     Res<Clock> clk) {
                     Goal const G = *goal;
                     // Strong pull only while steering; gentle while wandering (else it
                     // rings).
                     float const goalw  = cur->active ? T.goalFollow : T.goalSeek;
                     float const t      = clk->t;
                     FlockGrid const& g = *grid;
                     q.for_each_chunk([G, goalw, t, &g, T](std::span<Entity>,
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
                             int nn       = 0;
                             float spx = 0, spy = 0, spz = 0, svx = 0, svy = 0, svz = 0;
                             int const nb[7] = {c,
                                                g.widx(ix - 1, iy, iz),
                                                g.widx(ix + 1, iy, iz),
                                                g.widx(ix, iy - 1, iz),
                                                g.widx(ix, iy + 1, iz),
                                                g.widx(ix, iy, iz - 1),
                                                g.widx(ix, iy, iz + 1)};
                             for (int k : nb) {
                                 nn += g.count[k];
                                 spx += g.sx[k];
                                 spy += g.sy[k];
                                 spz += g.sz[k];
                                 svx += g.vx[k];
                                 svy += g.vy[k];
                                 svz += g.vz[k];
                             }
                             float const invn =
                                 1.0f / float(nn); // nn >= 1 (self counted)

                             // Cohesion: toward the local centre of mass.
                             add_dir(ax,
                                     ay,
                                     az,
                                     spx * invn - x,
                                     spy * invn - y,
                                     spz * invn - z,
                                     T.cohesion);
                             // Alignment: toward the local mean heading.
                             add_dir(ax, ay, az, svx, svy, svz, T.alignment);

                             // Separation: only crowded cells push down the density
                             // gradient.
                             if (float(g.count[c]) > T.softCap) {
                                 float const sgx = float(g.count_at(ix - 1, iy, iz) -
                                                         g.count_at(ix + 1, iy, iz));
                                 float const sgy = float(g.count_at(ix, iy - 1, iz) -
                                                         g.count_at(ix, iy + 1, iz));
                                 float const sgz = float(g.count_at(ix, iy, iz - 1) -
                                                         g.count_at(ix, iy, iz + 1));
                                 add_dir(ax, ay, az, sgx, sgy, sgz, T.separation);
                             }

                             // Goal seek: the flock flies toward the goal -- the
                             // wandering roost, or the cursor when you are steering (set
                             // in `goal`). The weight (goalw) is stronger while steering
                             // so it tracks closely without ringing up the hands-off
                             // wander.
                             add_dir(ax, ay, az, G.x - x, G.y - y, G.z - z, goalw);

                             // Wander: per-bird 3D turbulence -- neighbours diverge a
                             // little, which breaks thin lines/sheets into volume and
                             // adds a living shimmer.
                             float const k = cfg::kWanderFreq;
                             add_dir(ax,
                                     ay,
                                     az,
                                     std::sin(k * y + 0.7f * t) +
                                         std::sin(1.7f * k * z - 0.5f * t),
                                     std::sin(k * z + 0.6f * t) +
                                         std::sin(1.7f * k * x + 0.4f * t),
                                     std::sin(k * x - 0.8f * t) +
                                         std::sin(1.7f * k * y + 0.5f * t),
                                     T.wander);

                             // Soft boundary in the view plane (xy): steer back past the
                             // wall.
                             float const r2 = x * x + y * y;
                             if (r2 > cfg::kBound * cfg::kBound) {
                                 float const r = std::sqrt(r2);
                                 add_dir(ax,
                                         ay,
                                         az,
                                         -x,
                                         -y,
                                         0.0f,
                                         cfg::kBoundForce * (r - cfg::kBound));
                             }
                             // Depth band: hold the flock at a readable size -- not
                             // receding into the far haze (kZBack), nor drifting forward
                             // of kZFront... except the front cap follows a forward goal
                             // dive (G.z), so the flock can chase the goal toward the
                             // camera during a fly-over.
                             if (z < cfg::kZBack)
                                 add_dir(ax,
                                         ay,
                                         az,
                                         0.0f,
                                         0.0f,
                                         1.0f,
                                         cfg::kBoundForce * (cfg::kZBack - z));
                             float const zfront = std::max(cfg::kZFront, G.z + 0.25f);
                             if (z > zfront)
                                 add_dir(ax,
                                         ay,
                                         az,
                                         0.0f,
                                         0.0f,
                                         -1.0f,
                                         cfg::kBoundForce * (z - zfront));

                             // Apply the steering, then pull speed back to the cruise so
                             // the birds always fly (turning, not speeding up or
                             // stopping).
                             float nvx = vx[i] + ax * cfg::kDt;
                             float nvy = vy[i] + ay * cfg::kDt;
                             float nvz = vz[i] + az * cfg::kDt;
                             float const sp =
                                 std::sqrt(nvx * nvx + nvy * nvy + nvz * nvz);
                             if (sp > 1e-5f) {
                                 float const f = T.cruise / sp;
                                 nvx *= f;
                                 nvy *= f;
                                 nvz *= f;
                             }
                             vx[i] = nvx;
                             vy[i] = nvy;
                             vz[i] = nvz;
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

    // metrics (optional, ECS_METRICS=<path.csv>): log whole-flock dynamics each
    // tick so behaviour can be reviewed *quantitatively over time* (not by eye on
    // a single frame). Columns: sim time; the centre of mass projected to screen
    // (com_sx,com_sy) and its depth; the goal projected to screen (= the cursor
    // when steering); follow_err = screen distance from the flock's centre to the
    // goal (0 = sitting on it; a non-decaying oscillation = the undamped wheel);
    // spread = RMS world radius of the flock (compact vs diffuse); coherence =
    // |mean velocity| / mean speed in [0,1] (1 = flying together, ~0 = wheeling /
    // scrambled). Two cheap serial passes, only when enabled. Reads settled
    // positions, so it lands in the last wave.
    if (char const* mpath = std::getenv("ECS_METRICS")) {
        auto out = std::make_shared<std::ofstream>(mpath);
        *out << "t,com_sx,com_sy,com_z,goal_sx,goal_sy,follow_err,spread,coherence\n";
        schedule.add("metrics",
                     [out](Query<Position const, Velocity const> q,
                           Res<Goal> goal,
                           Res<Clock> clk) {
                         float cx = 0, cy = 0, cz = 0, vx = 0, vy = 0, vz = 0, spd = 0;
                         int n = 0;
                         q.each([&](Entity, Position const& p, Velocity const& v) {
                             cx += p.x;
                             cy += p.y;
                             cz += p.z;
                             vx += v.x;
                             vy += v.y;
                             vz += v.z;
                             spd += std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                             ++n;
                         });
                         if (n == 0)
                             return;
                         float const inv = 1.0f / float(n);
                         float const mcx = cx * inv, mcy = cy * inv, mcz = cz * inv;
                         float const mspd = spd * inv;
                         float svar       = 0;
                         q.each([&](Entity, Position const& p, Velocity const&) {
                             float const dx = p.x - mcx, dy = p.y - mcy, dz = p.z - mcz;
                             svar += dx * dx + dy * dy + dz * dz;
                         });
                         float const spread = std::sqrt(svar * inv);
                         // CoM/goal -> screen. Floor the depth at the cull plane:
                         // a deep fly-over can carry the CoM past the camera
                         // (kCamZ - z <= 0), which would divide by ~0 and flip.
                         float const ci =
                             cfg::kFocal / std::max(cfg::kNearClip, cfg::kCamZ - mcz);
                         float const gi =
                             cfg::kFocal / std::max(cfg::kNearClip, cfg::kCamZ - goal->z);
                         float const csx = mcx * ci, csy = mcy * ci;
                         float const gsx = goal->x * gi, gsy = goal->y * gi;
                         float const ferr = std::sqrt((csx - gsx) * (csx - gsx) +
                                                      (csy - gsy) * (csy - gsy));
                         float const coh =
                             mspd > 1e-5f
                                 ? std::sqrt(vx * vx + vy * vy + vz * vz) * inv / mspd
                                 : 0.0f;
                         *out << clk->t << ',' << csx << ',' << csy << ',' << mcz << ','
                              << gsx << ',' << gsy << ',' << ferr << ',' << spread << ','
                              << coh << '\n';
                         out->flush(); // headless runs are killed by timeout; don't lose
                                       // buffered rows
                     });
    }
}
