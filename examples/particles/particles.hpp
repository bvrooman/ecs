// examples/particles/particles.hpp
//
// Shared domain types for the particle example: the plain-struct components and
// resources the ECS stores, the draw-ready vertex that crosses the sim/render
// thread boundary, and the simulation tuning knobs. Included by both the
// simulation (simulation.cpp) and the renderer (main.cpp).
#pragma once

#include <random>
#include <vector>

// --- components: an entity is whatever components it carries ----------------
struct Position {
    float x, y;
};
struct Velocity {
    float x, y;
};
struct Color {
    float r, g, b;
};
struct Age {
    float t, max;
}; // seconds lived / lifespan before reaping

// Per-particle force from three independent turbulence fields. Kept as separate
// components (not one) so their systems write disjoint data and the scheduler
// can run them in parallel -- the heavier workload that lets the pool pay off.
struct Swirl {
    float x, y;
};
struct Drift {
    float x, y;
};
struct Gust {
    float x, y;
};

// --- resources (singletons, not attached to any entity) --------------------
struct Gravity {
    float accel;
}; // read by the gravity system (Res<>)
struct Rng {
    std::mt19937 gen;
}; // written by the emitter      (ResMut<>)
struct Clock {
    float t;
}; // sim time, advanced each tick; drives the animated force fields

// Runtime-tunable emitter knobs: spawn count per tick and nozzle position. Held
// as a resource (not cfg:: constants) so a host can adjust them live -- the web
// demo drives these from sliders and the cursor. Defaults mirror the cfg:: values.
struct Emitter {
    int per_tick;
    float origin_x, origin_y;
};

// One draw-ready vertex: clip-space position + RGBA. This is what crosses the
// thread boundary; the ECS knows nothing about GL.
struct GpuParticle {
    float x, y, r, g, b, a;
};
using RenderSnapshot = std::vector<GpuParticle>;

// --- simulation tuning ------------------------------------------------------
namespace cfg {
inline constexpr int kSimHz       = 120;           // simulation ticks per second
inline constexpr float kDt        = 1.0f / kSimHz; // fixed timestep
inline constexpr int kEmitPerTick = 20;            // new particles each tick
inline constexpr float kGravity   = -1.7f;         // clip units / s^2
inline constexpr float kOriginX   = 0.0f;
inline constexpr float kOriginY   = -0.85f; // nozzle near the bottom edge
// Turbulence: each of the three field systems runs kTurbOctaves octaves of curl
// noise per particle -- the compute-bound, embarrassingly parallel work that
// makes the std::execution pool worthwhile. Raise to stress the scheduler
// harder; 0 makes the fields free (back to the plain fountain).
inline constexpr int kTurbOctaves    = 4;
inline constexpr float kTurbStrength = 0.8f;
} // namespace cfg
