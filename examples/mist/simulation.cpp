// examples/mist/simulation.cpp
//
// A starling murmuration as a schedule of ECS systems -- and the library's
// proving ground for KERNEL systems and the parallel-primitives suite. Access
// is derived from each system's parameter types and leveled into waves
// automatically:
//
//   phase -1 (every 64 ticks): sort-rows -> re-sort by grid cell at the barrier
//   wave 0:   input | clock | grid   (grid = kernel Reduce, sparse partials)
//   wave 1:   goal                   (after input's Cursor + clock's Clock)
//   wave 2:   steer                  (kernel: boids + goal -> Velocity)
//   wave 3:   integrate              (kernel: Velocity -> Position)
//   wave 4:   extract [| metrics]    (kernel, Extract into the back buffer)
//   wave 5:   publish [| metrics-write]
//
// The flock is seeded once at load with seed_flock() (NOT a scatter-on-tick-0
// system), then the schedule is prewarmed, so every tick the schedule runs is
// a steady per-bird tick with no cold-start outlier -- the load/frame-loop
// split a real game uses; see simulation.hpp and the callers.
//
// The division of labour the port demonstrates: per-bird data-parallel work
// (`grid`, `steer`, `integrate`, `extract`, `metrics`) is registered with
// add_kernel, so the executor slices each system's rows into work items and
// overlaps everything in a wave across the pool's lanes -- while the small
// ORDERED resource writers (`input`, `clock`, `goal`, `publish`) stay
// imperative add() systems. Where a serial system gathered shared output, the
// kernels use the barrier-merged primitives instead:
//
//   grid     Reduce<FlockGrid, MergeCells, CellAccum>: sparse touched-cells
//            partials folded into the dense grid at the barrier -- viable
//            only because the sort-rows system keeps rows
//            key-coherent (see the `grid` comment for the measured story).
//   extract  Extract<SnapshotTarget>: disjoint per-item spans write the GPU
//            vertices straight into the triple buffer's back buffer (the
//            old serial gather, minus the serialization and the copy).
//   metrics  Reduce<FlockStats, AddStats>: one pass over every bird instead
//            of the old two serial passes.
//
// Because every merge folds in canonical order, the row slicing is
// lane-count independent, and the periodic sort is stable over a
// canonical key, the simulation's evolution is IDENTICAL at 1 lane and N --
// same seed, same flock, bit for bit.
//
// `grid` reduces every bird into a FlockGrid resource (per-cell density + sums
// of position and velocity); `steer` (the compute-bound, data-parallel per-bird
// kernel) reads each bird's cell + 6 face neighbours to apply Reynolds boids --
// cohesion (toward the local centre of mass), alignment (toward the local mean
// heading) and separation (away from crowding) -- plus a pull toward the slowly
// wandering `goal` (which flies the whole flock around, and which the cursor
// takes over when you steer). The accumulated steering is applied as
// an acceleration, then speed is pulled back to a cruise: steering turns the
// birds, it does not speed them up or stop them, which is what makes the flock
// read as *flight* rather than drifting. Commands flush at each wave barrier.
#include "simulation.hpp"

#include <ecs/ecs.hpp>

#include "mist.hpp"
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <utility>

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

// Fast sine for the per-bird wander turbulence (6 calls/bird -- the tick's hot
// spot; ~half of steer). The wander is aesthetic noise, not precision-critical,
// so a range-reduced parabola approximation (Nick's parabola + the standard
// refinement, ~0.1% max error) is visually indistinguishable from std::sin and
// ~2x cheaper. NOT used for the goal's Lissajous (per-tick, few calls, wants the
// real thing). Range-reduce to [-pi, pi] first since the phase (c*t) grows.
inline float fast_sin(float x) {
    constexpr float kInvTwoPi = 0.15915494f; // 1/(2*pi)
    constexpr float kTwoPi    = 6.2831853f;
    x -= kTwoPi * std::nearbyint(x * kInvTwoPi);      // -> [-pi, pi]
    constexpr float B = 1.2732395f;                   // 4/pi
    constexpr float C = -0.4052847f;                  // -4/pi^2
    float const y     = B * x + C * x * std::fabs(x); // parabola (~6% error)
    return 0.225f * (y * std::fabs(y) - y) + y;       // refine -> ~0.1% error
}

// Runtime-overridable steering weights, so the flight can be swept/tuned without
// recompiling (e.g. ECS_GOALSEEK=1.6 ECS_SOFTCAP=60 ./mist). Defaults are the
// cfg values; the grid and camera stay compile-time.
struct Tuning {
    float cruise, goalSeek, goalFollow, cohesion, alignment, separation, softCap, wander;
    float dive; // fly-over dive depth
};

template <typename T>
inline T env(char const* key, T dflt) {
    char const* v = std::getenv(key);
    return (v && *v) ? static_cast<T>(std::atof(v)) : dflt;
}
inline Tuning read_tuning() {
    return Tuning {env("ECS_CRUISE", cfg::kCruise),
                   env("ECS_GOALSEEK", cfg::kGoalSeek),
                   env("ECS_GOALFOLLOW", cfg::kGoalFollow),
                   env("ECS_COHESION", cfg::kCohesion),
                   env("ECS_ALIGN", cfg::kAlignment),
                   env("ECS_SEPARATION", cfg::kSeparation),
                   env("ECS_SOFTCAP", cfg::kSoftCap),
                   env("ECS_WANDER", cfg::kWander),
                   env("ECS_DIVE", cfg::kDiveAmp)};
}

// The grid kernel's per-item partial: entries for only the cells this item's
// birds touch, plus a cell -> entry index so each bird's add is ONE array
// lookup (a full dense FlockGrid per item would be cfg::kGridDim^3 cells,
// megabytes each; a hash map measured slower than the serial rebuild). Both
// structures persist per item; clear() re-zeroes only the touched index
// slots. This partial compresses -- and the kernel wins -- ONLY because the
// sort-rows system keeps row order correlated with cell index; see
// the `grid` registration below for the measured story.
struct CellAccum {
    std::vector<int> idx; // cell -> entry index + 1; 0 = untouched
    std::vector<std::pair<int, Cell>> entries;

    // Allocate the dense index eagerly (256 KB for a 40^3 grid), NOT lazily on
    // first at(): with ~64 partials this is ~16 MB, and paying it on the first
    // tick's item execution was that tick's dominant cost (~3 ms). Constructing
    // it here means Schedule::prewarm -- which sizes the Reduce slot array
    // before the timed loop -- pre-pays it, so the first real tick is steady.
    CellAccum()
        : idx(std::size_t(FlockGrid::cells), 0) {}

    Cell& at(int const c) {
        int& slot = idx[std::size_t(c)];
        if (slot == 0) {
            entries.emplace_back(c, Cell {});
            slot = int(entries.size());
        }
        return entries[std::size_t(slot - 1)].second;
    }
    void clear() {
        for (auto const& [c, agg] : entries)
            idx[std::size_t(c)] = 0;
        entries.clear();
    }
};
struct MergeCells {
    void operator()(FlockGrid& into, CellAccum& part) const {
        for (auto const& [c, agg] : part.entries) {
            auto& d = into.cell[std::size_t(c)];
            d.count += agg.count;
            d.sx += agg.sx;
            d.sy += agg.sy;
            d.sz += agg.sz;
            d.vx += agg.vx;
            d.vy += agg.vy;
            d.vz += agg.vz;
        }
    }
};

// Fold for the metrics reduction: plain field-wise sums.
struct AddStats {
    void operator()(FlockStats& a, FlockStats& b) const {
        a.x += b.x;
        a.y += b.y;
        a.z += b.z;
        a.vx += b.vx;
        a.vy += b.vy;
        a.vz += b.vz;
        a.speed += b.speed;
        a.r2 += b.r2;
        a.n += b.n;
    }
};

} // namespace

// Row-sort key: the grid cell a bird falls in. Sorting rows by it keeps
// birds in the same cell adjacent in storage, so grid's sparse CellAccum
// partials touch a compact cell range (they compress, and the kernel wins).
// Shared by the load-time sort in seed_flock() and the periodic sort
// hook in build_mist_schedule(), so both use the identical key.
static int flock_cell(Position const& p) {
    return FlockGrid::index(FlockGrid::axis(p.x),
                            FlockGrid::axis(p.y),
                            FlockGrid::axis(p.z));
}

// The flock-seeding body: place the birds in a loose swirling disc. Invoked by
// seed_flock() (the load step); factored out so the RNG-draw order stays fixed.
static void scatter_flock(Commands& cmd, Rng& rng, int const count) {
    auto& g = rng.gen;
    std::uniform_real_distribution<float> p(-0.45f, 0.45f);
    std::uniform_real_distribution<float> j(-0.06f, 0.06f);
    // Start as a gently swirling disc centred on screen, at the working depth
    // (~kRoamZc). The velocity is a curl (rotational) field, v ~ (-y, x):
    // locally coherent -- neighbours share a heading, so alignment stays happy
    // and it reads as a murmuration from t=0 -- but its centre of mass stays
    // put. A single shared heading instead translated the whole flock
    // off-screen (it rushed to the top-edge before the goal could rein it
    // back); the swirl lingers in the middle.
    for (int i = 0; i < count; ++i) {
        float const px = p(g), py = p(g);
        // Swirl (-y, x) + a gentle shared forward drift (+z): the drift gives
        // the dead-centre birds (whose swirl velocity is ~0) a coherent heading
        // too, so the centre doesn't evacuate into a ring.
        cmd.spawn(Position {px, py, cfg::kRoamZc + p(g)},
                  Velocity {-py + j(g), px + j(g), 0.20f + j(g)});
    }
}

void seed_flock(World& world, int const count) {
    Schedule s;
    s.add_once("seed_flock", [count](Commands& cmd, ResMut<Rng> rng) {
        scatter_flock(cmd, *rng, count);
    });
    s.run(world); // one flush spawns the birds into their archetype
    // Sort by grid cell now, so the flock starts as row-coherent as the
    // sort-rows system keeps it. Spawn order is uncorrelated with cell, so
    // without this the first ~64 ticks (before the first periodic sort) run on
    // disordered rows and grid's partials don't compress -- a warm-up hump of
    // ~2x the steady busy that resolves only at the first periodic sort.
    world.sort_rows<Position>(flock_cell);
}

void build_mist_schedule(Schedule& schedule, MistInput in) {
    Tuning const T = read_tuning(); // steering weights (env-overridable, for sweeps)

    // input: copy the latest cursor into the Cursor resource on the sim thread
    // (single-writer); `goal`/`steer` read it. wave 0.
    schedule.add("input", [in](ResMut<Cursor> cur) {
        std::uint64_t const c = in.cursor->load(std::memory_order_relaxed);
        std::uint32_t const f = in.flags->load(std::memory_order_relaxed);
        cur->x                = cursor_x(c);
        cur->y                = cursor_y(c);
        cur->active           = (f & 1u) != 0;
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

    // sort-rows: every 64 ticks, re-sort birds by grid cell so row order stays
    // correlated with the spatial key (swap-and-pop churn and flight both decay
    // it slowly). A plain phase<-1> system that records a sort COMMAND, so the
    // permute runs at that phase's barrier -- world-quiescent, single-threaded,
    // before the tick's other waves -- with the schedule's normal timing,
    // reporting, and exception handling (no bespoke maintenance path). This is
    // the data-layout precondition that makes the grid KERNEL below pay: work
    // items slice contiguous row ranges, so key-coherent rows are what let its
    // per-item partials compress. Stable sort, canonical key: the permutation is
    // identical at every lane count, so the sim stays bitwise reproducible
    // (mist-headless asserts it). Counting-sort path: ~0.9ms per 40k birds,
    // ~14us/tick amortized at this cadence.
    schedule.add(
        "sort-rows",
        [](Commands& c) { c.sort<Position>(flock_cell); },
        phase<-1> {},
        /*every=*/64);

    // grid: reduce every bird into the spatial grid (per-cell count + position
    // and velocity sums). A KERNEL whose Reduce partial is a sparse
    // touched-cells accumulator -- the shape that LOSES on uncorrelated rows
    // (measured ~2x the serial rebuild at 1 lane: partials don't compress, so
    // the barrier fold re-did ~n adds serially) and WINS once the sort-rows
    // system above keeps rows key-coherent (measured ~2x FASTER than the serial
    // rebuild at 4 lanes: each item touches a compact cell range, the fold
    // shrinks to a few thousand entries, and the accumulation parallelizes).
    // The general rule lives in docs/IMPROVEMENTS.md: partial+merge pays when
    // partials compress or per-row work dominates; sorting is how a cheap
    // scatter-add is MADE to compress. The registration is a bet on lanes:
    // at 1 lane the indirection still costs ~10% of the tick vs the serial
    // rebuild; at 4 lanes the whole tick is ~7% faster and scales further.
    schedule.add_kernel("grid",
                        [](Query<Position const, Velocity const> q,
                           Reduce<FlockGrid, MergeCells, CellAccum> g) {
                            q.for_each([&](auto& p, auto& v) {
                                int const c = FlockGrid::index(FlockGrid::axis(p.x),
                                                               FlockGrid::axis(p.y),
                                                               FlockGrid::axis(p.z));
                                auto& [count, sx, sy, sz, vx, vy, vz] = g->at(c);
                                count += 1;
                                sx += p.x;
                                sy += p.y;
                                sz += p.z;
                                vx += v.x;
                                vy += v.y;
                                vz += v.z;
                            });
                        });

    // steer: turn each bird. Accumulate the boids rules + goal into a steering
    // acceleration, apply it, then pull speed back to the cruise. (The cursor
    // acts through the goal above, so there is no per-bird cursor force here.)
    // A KERNEL -- the compute-bound heart of the tick: components-in-the-Query
    // plus read-only resources, per-row independent, exactly the shape
    // add_kernel slices. The body is untouched from the imperative version;
    // the registration changed one word (add -> add_kernel) and the body
    // iterates with for_each (the executor parallelizes ACROSS the items,
    // and each item's slice iterates inline on its claiming lane).
    schedule.add_kernel(
        "steer",
        [T](Query<Position const, Velocity> q,
            Res<FlockGrid> grid,
            Res<Goal> goal,
            Res<Cursor> cur,
            Res<Clock> clk) {
            Goal const G       = *goal;
            float const goalw  = cur->active ? T.goalFollow : T.goalSeek;
            float const t      = clk->t;
            FlockGrid const& g = *grid;
            q.for_each([G, goalw, t, &g, T](auto& p, auto& v) {
                float const x = p.x, y = p.y, z = p.z;
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
                    auto const& [count, sx, sy, sz, vx, vy, vz] = g.cell[k];
                    nn += count;
                    spx += sx;
                    spy += sy;
                    spz += sz;
                    svx += vx;
                    svy += vy;
                    svz += vz;
                }
                float const invn = 1.0f / float(nn); // nn >= 1 (self counted)

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
                if (float(g.cell[c].count) > T.softCap) {
                    auto const sgx =
                        float(g.count_at(ix - 1, iy, iz) - g.count_at(ix + 1, iy, iz));
                    auto const sgy =
                        float(g.count_at(ix, iy - 1, iz) - g.count_at(ix, iy + 1, iz));
                    auto const sgz =
                        float(g.count_at(ix, iy, iz - 1) - g.count_at(ix, iy, iz + 1));
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
                auto const dx =
                    fast_sin(k * y + 0.7f * t) + fast_sin(1.7f * k * z - 0.5f * t);
                auto const dy =
                    fast_sin(k * z + 0.6f * t) + fast_sin(1.7f * k * x + 0.4f * t);
                auto const dz =
                    fast_sin(k * x - 0.8f * t) + fast_sin(1.7f * k * y + 0.5f * t);
                add_dir(ax, ay, az, dx, dy, dz, T.wander);

                // Soft boundary in the view plane (xy): steer back past the
                // wall.
                if (float const r2 = x * x + y * y; r2 > cfg::kBound * cfg::kBound) {
                    float const r = std::sqrt(r2);
                    float const w = cfg::kBoundForce * (r - cfg::kBound);
                    add_dir(ax, ay, az, -x, -y, 0.0f, w);
                }
                // Depth band: hold the flock at a readable size -- not
                // receding into the far haze (kZBack), nor drifting forward
                // of kZFront... except the front cap follows a forward goal
                // dive (G.z), so the flock can chase the goal toward the
                // camera during a fly-over.
                if (z < cfg::kZBack) {
                    float const w = cfg::kBoundForce * (cfg::kZBack - z);
                    add_dir(ax, ay, az, 0.0f, 0.0f, 1.0f, w);
                }
                if (float const zfront = std::max(cfg::kZFront, G.z + 0.25f);
                    z > zfront) {
                    float const w = cfg::kBoundForce * (z - zfront);
                    add_dir(ax, ay, az, 0.0f, 0.0f, -1.0f, w);
                }

                // Apply the steering, then pull speed back to the cruise so
                // the birds always fly (turning, not speeding up or
                // stopping).
                float nvx = v.x + ax * cfg::kDt;
                float nvy = v.y + ay * cfg::kDt;
                float nvz = v.z + az * cfg::kDt;
                if (float const sp = std::sqrt(nvx * nvx + nvy * nvy + nvz * nvz);
                    sp > 1e-5f) {
                    float const f = T.cruise / sp;
                    nvx *= f;
                    nvy *= f;
                    nvz *= f;
                }
                v.x = nvx;
                v.y = nvy;
                v.z = nvz;
            });
        });

    // integrate: advance Position by Velocity. No wrap -- the flock flies free,
    // held in the roaming region by steer's soft boundary. A kernel: pure
    // per-row map over the components in the Query.
    schedule.add_kernel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x * cfg::kDt;
            p.y += v.y * cfg::kDt;
            p.z += v.z * cfg::kDt;
        });
    });

    // extract: gather a draw-ready snapshot of every bird's position (the
    // shader culls/fades by depth). A kernel with Extract: the SnapshotTarget
    // resource forwards resize/data to the triple buffer's back buffer, so
    // the executor pre-sizes it once at the barrier and each item writes its
    // disjoint span of GPU vertices in place -- the old serial gather, spread
    // across the wave, with no intermediate copy.
    schedule.add_kernel("extract",
                        [](Query<Position const> q, Extract<SnapshotTarget> out) {
                            q.for_each_chunk([&](std::span<Entity const>,
                                                 chunk<Position const> p) {
                                auto const x = p.column<0>();
                                auto const y = p.column<1>();
                                auto const z = p.column<2>();
                                for (std::size_t i = 0; i < x.size(); ++i)
                                    out[i] = GpuParticle {x[i], y[i], z[i]};
                            });
                        });

    // publish: flip the finished back buffer to the renderer. Ordered small
    // work -- an imperative system. Reading SnapshotTarget places it after
    // extract's write at the next barrier, when the spans are complete.
    schedule.add("publish",
                 [](Res<SnapshotTarget>, ResMut<TripleBuffer<RenderSnapshot>> ch) {
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
    // scrambled). Split kernel/serial along the same line as the rest of the
    // schedule: `metrics` reduces every bird into FlockStats in ONE pass (the
    // old version needed two serial passes because variance wanted the mean
    // first; summing |p|^2 alongside gets spread from E[|p|^2] - |mean|^2),
    // and `metrics-write` derives the columns and writes the CSV row --
    // ordered side effects, an imperative system, leveled after the reduce by
    // its FlockStats read. Reads settled positions, so both land in the last
    // waves. Only registered when enabled.
    if (char const* mpath = std::getenv("ECS_METRICS")) {
        schedule.add_kernel("metrics",
                            [](Query<Position const, Velocity const> q,
                               Reduce<FlockStats, AddStats> st) {
                                q.for_each([&](auto& p, auto& v) {
                                    st->x += p.x;
                                    st->y += p.y;
                                    st->z += p.z;
                                    st->vx += v.x;
                                    st->vy += v.y;
                                    st->vz += v.z;
                                    st->speed +=
                                        std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
                                    st->r2 += p.x * p.x + p.y * p.y + p.z * p.z;
                                    st->n += 1;
                                });
                            });

        auto out = std::make_shared<std::ofstream>(mpath);
        *out << "t,com_sx,com_sy,com_z,goal_sx,goal_sy,follow_err,spread,coherence\n";
        schedule.add("metrics-write",
                     [out](Res<FlockStats> st, Res<Goal> goal, Res<Clock> clk) {
                         if (st->n == 0)
                             return;
                         double const inv = 1.0 / double(st->n);
                         double const mcx = st->x * inv, mcy = st->y * inv,
                                      mcz  = st->z * inv;
                         double const mspd = st->speed * inv;
                         // RMS radius about the mean in one pass: E[|p|^2] - |mean|^2
                         // (clamped: the difference of two large sums can go a hair
                         // negative in floating point when the flock is very tight).
                         double const svar =
                             st->r2 * inv - (mcx * mcx + mcy * mcy + mcz * mcz);
                         float const spread = float(std::sqrt(std::max(0.0, svar)));
                         // CoM/goal -> screen. Floor the depth at the cull plane:
                         // a deep fly-over can carry the CoM past the camera
                         // (kCamZ - z <= 0), which would divide by ~0 and flip.
                         float const ci = cfg::kFocal / std::max(cfg::kNearClip,
                                                                 cfg::kCamZ - float(mcz));
                         float const gi =
                             cfg::kFocal / std::max(cfg::kNearClip, cfg::kCamZ - goal->z);
                         float const csx = float(mcx) * ci, csy = float(mcy) * ci;
                         float const gsx = goal->x * gi, gsy = goal->y * gi;
                         float const ferr = std::sqrt((csx - gsx) * (csx - gsx) +
                                                      (csy - gsy) * (csy - gsy));
                         double const mv  = std::sqrt(st->vx * st->vx + st->vy * st->vy +
                                                      st->vz * st->vz);
                         float const coh  = mspd > 1e-5 ? float(mv * inv / mspd) : 0.0f;
                         *out << clk->t << ',' << csx << ',' << csy << ',' << float(mcz)
                              << ',' << gsx << ',' << gsy << ',' << ferr << ',' << spread
                              << ',' << coh << '\n';
                         out->flush(); // headless runs are killed by timeout; don't lose
                                       // buffered rows
                     });
    }
}
