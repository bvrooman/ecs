// examples/mist/mist.hpp
//
// Shared domain types for the "mist" example: a starling **murmuration**. A
// free-flying flock of thousands of tiny birds wheels and folds through a 3D
// volume, holding together by boids rules (cohesion/alignment/separation) while
// a slowly wandering global goal flies the whole mass around the scene -- and
// sometimes straight over the viewer. Rendered as tiny black specks in close
// perspective over white; the eerie light/dark density waves emerge on their own
// from how many birds overlap along each view ray (alpha-over = absorption).
// Included by both the simulation (simulation.cpp) and the renderer (main.cpp).
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <random>
#include <vector>

// --- components: an entity is whatever components it carries ----------------
struct Position {
    float x, y, z;
};
struct Velocity {
    float x, y, z;
};

// --- resources (singletons, not attached to any entity) ---------------------
struct Rng {
    std::mt19937 gen;
}; // written by the one-shot scatter (ResMut<>)
struct Clock {
    float t;
}; // sim time, advanced each tick; drives the wandering goal
struct Goal {
    float x, y, z;
}; // the moving point the whole flock flies toward (the "intention")

// The cursor, in clip space (-1..1), as a *predator* the flock veers away from.
// Written each tick by `input`; `aspect` keeps the avoided region round.
struct Cursor {
    float x = 0.0f, y = 0.0f;
    bool active  = false;
    float aspect = 1.0f;
};

// One draw-ready vertex: just the 3D position (colour is a constant black; depth
// shade/fade is computed in the shader). Crosses the sim/render thread boundary.
struct GpuParticle {
    float x, y, z;
};
using RenderSnapshot = std::vector<GpuParticle>;

// --- simulation tuning ------------------------------------------------------
namespace cfg {
// 60 Hz fixed step. The motion animates in wall time (Clock advances kDt/tick),
// so the look is independent of this rate.
inline constexpr int kSimHz = 60;
inline constexpr float kDt  = 1.0f / kSimHz;
inline constexpr int kCount = 85000; // birds (ECS_PARTICLES overrides)

// Camera (shared with the renderer): a fixed perspective view from +kCamZ
// looking toward -z, sitting close so the flock looms large as it sweeps near.
// Birds nearer than kNearClip (z past the camera) are culled -- they have flown
// over/behind the viewer. Lives here, not just in main, because `steer` needs it
// to push the flock away from the cursor's screen ray (the predator void).
inline constexpr float kCamZ     = 1.50f;
inline constexpr float kFocal    = 1.30f;
inline constexpr float kNearClip = 0.10f; // cull birds closer than this depth
inline constexpr float kFarFade  = 3.40f; // depth where birds haze fully out

// Flight: birds cruise at ~kCruise (steering changes *direction*, not speed --
// that is what makes it read as flight, not floating). Per-tick steering is the
// weighted sum of boids rules + the goal, applied as an acceleration, after
// which speed is pulled back to the cruise. Weights are dimensionless.
inline constexpr float kCruise   = 0.65f; // clip units / s
// Pull toward the goal. Two strengths because they want opposite things (a fact
// the metric sweep surfaced): the slow hands-off WANDER wants a gentle pull, or
// the constant-speed flock collapses into a tight orbiting ring; the CURSOR (a
// faster, user-driven target) wants a strong pull to actually track it. So the
// strong value is used only while steering. kGoalFollow ~2.0 was the sweep knee:
// follow_err ~0.1 on smooth motion without dispersing (see examples/mist/tools).
inline constexpr float kGoalSeek   = 1.00f; // pull toward the wandering roost (hands-off)
inline constexpr float kGoalFollow = 2.00f; // pull toward the cursor (while steering)
inline constexpr float kCohesion   = 0.95f; // toward the local centre of mass
inline constexpr float kAlignment  = 0.55f; // toward the local mean heading
inline constexpr float kSeparation = 0.65f; // away from local crowding
inline constexpr float kSoftCap    = 40.0f; // per-cell density before separation bites
// Per-bird turbulence: a 3D noise "wander" so neighbours diverge a little -- it
// breaks the flock out of thin lines/sheets and gives it 3D volume and a
// living shimmer. Gentle, so it adds body without dispersing the mass.
inline constexpr float kWander     = 0.30f;
inline constexpr float kWanderFreq = 1.9f;

// The cursor *steers the whole murmuration's direction* -- it is not a local
// attractor (that only ever made a swarm point). While the cursor is in the
// window it becomes the flock's goal (see the `goal` system): the whole mass
// turns and flies toward where you point, then resumes wandering when you leave.
// How hard it chases the cursor is just kGoalSeek (above) -- raise that for a
// snappier, more responsive steer, lower it for a lazier turn.

// The goal wanders on a slow Lissajous. Its xy targets are in *screen* space
// (bounded apparent position) and converted to world at the goal's depth, so
// the flock always flies around within view no matter how near it is; its z
// swings up near the camera so the murmuration periodically sweeps over the
// viewer. A soft boundary keeps stray birds from escaping the roaming region.
inline constexpr float kRoamX      = 0.60f; // screen-x amplitude (clip units)
inline constexpr float kRoamY      = 0.50f; // screen-y amplitude
inline constexpr float kRoamZc     = 0.10f; // mid depth of the goal's z swing
inline constexpr float kRoamZ      = 0.10f; // z amplitude (peak ~d=0.15: sweeps overhead)
inline constexpr float kBound      = 2.60f; // soft wall radius
inline constexpr float kBoundForce = 2.50f;

// Uniform spatial grid covering the roaming volume, for the boids neighbourhood
// (a bird's cell + its 6 face neighbours). The flock is a dense blob roaming a
// large grid, so most cells are empty; indices are clamped at the edges (the
// flock stays interior, kept in by the soft boundary).
inline constexpr float kGridHalf = 3.0f;
inline constexpr int kGridDim    = 50;
inline constexpr float kGridCell = 2.0f * kGridHalf / kGridDim;
inline constexpr float kGridMin  = -kGridHalf;
} // namespace cfg

// FlockGrid: a uniform 3D grid of per-cell aggregates rebuilt every tick -- per
// cell, the bird count and the sums of position and velocity. `steer` reads a
// bird's cell + 6 face neighbours to get a local centre of mass (cohesion),
// mean heading (alignment) and density gradient (separation) in O(1). Installed
// as a resource; `grid` fills it.
struct FlockGrid {
    static constexpr int dim   = cfg::kGridDim;
    static constexpr int cells = dim * dim * dim;

    std::vector<std::uint16_t> count;
    std::vector<float> sx, sy, sz; // sum of positions per cell
    std::vector<float> vx, vy, vz; // sum of velocities per cell

    FlockGrid()
        : count(cells, 0)
        , sx(cells, 0)
        , sy(cells, 0)
        , sz(cells, 0)
        , vx(cells, 0)
        , vy(cells, 0)
        , vz(cells, 0) {}

    static int clampi(int i) { return i < 0 ? 0 : (i >= dim ? dim - 1 : i); }
    static int axis(float c) { return clampi(int((c - cfg::kGridMin) / cfg::kGridCell)); }
    static int index(int ix, int iy, int iz) { return (ix * dim + iy) * dim + iz; }

    // Clamped cell index for a (possibly out-of-range) neighbour offset -- use
    // it to read any per-cell array over the grid (edges read the border cell,
    // which is empty because the flock stays interior).
    [[nodiscard]]
    int widx(int ix, int iy, int iz) const {
        return index(clampi(ix), clampi(iy), clampi(iz));
    }
    [[nodiscard]]
    int count_at(int ix, int iy, int iz) const {
        return count[widx(ix, iy, iz)];
    }

    void clear() {
        std::fill(count.begin(), count.end(), std::uint16_t(0));
        std::fill(sx.begin(), sx.end(), 0.0f);
        std::fill(sy.begin(), sy.end(), 0.0f);
        std::fill(sz.begin(), sz.end(), 0.0f);
        std::fill(vx.begin(), vx.end(), 0.0f);
        std::fill(vy.begin(), vy.end(), 0.0f);
        std::fill(vz.begin(), vz.end(), 0.0f);
    }
};

// --- main -> worker-thread handoff (lock-free, no GL types) ------------------
struct WindowState {
    std::atomic<std::uint64_t> fb_size {0}; // (uint64(w) << 32) | uint32(h)
    std::atomic<std::uint64_t> cursor {0};  // packed clip-space (x, y)
    std::atomic<std::uint32_t> flags {0};   // bit 0: cursor inside the window
};

inline std::uint64_t pack_size(int w, int h) {
    return (std::uint64_t(std::uint32_t(w)) << 32) | std::uint32_t(h);
}
inline std::uint64_t pack_cursor(float x, float y) {
    return (std::uint64_t(std::bit_cast<std::uint32_t>(x)) << 32) |
           std::bit_cast<std::uint32_t>(y);
}
inline float cursor_x(std::uint64_t p) {
    return std::bit_cast<float>(std::uint32_t(p >> 32));
}
inline float cursor_y(std::uint64_t p) { return std::bit_cast<float>(std::uint32_t(p)); }

struct MistInput {
    std::atomic<std::uint64_t> const* cursor  = nullptr;
    std::atomic<std::uint32_t> const* flags   = nullptr;
    std::atomic<std::uint64_t> const* fb_size = nullptr;
};
