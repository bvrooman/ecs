// examples/nbody/nbody.hpp
//
// The data model of the naive n-body reference: components, units, the flat
// body array the force kernel scans, and the conservation-tracking resources.
// No systems and no harness -- see simulation.cpp and main.cpp.
#pragma once

#include <cstdint>
#include <vector>

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
// G = 1, total mass = 1, initial radius = 1. Everything is in those units, so a
// crossing time is ~1 and the printed numbers stay readable.
namespace cfg {
constexpr float kG      = 1.0f;
constexpr float kRadius = 1.0f;
constexpr float kSoft   = 0.02f; // Plummer epsilon
constexpr float kSoft2  = kSoft * kSoft;
constexpr float kDt     = 0.004f;
}

// The flat array the force kernel sums over: the whole system, rebuilt every
// tick by `gather`. AoS (not SoA) on purpose -- the inner loop reads all four
// fields of body j together, so one 16-byte line per j is the tightest access
// pattern available.
struct Body {
    float x, y, z, m;
};
using BodyArray = std::vector<Body>;

// --- reduction targets ------------------------------------------------------
// Separate resources because they are separate quantities, produced by
// different systems. Both accumulate in double -- summing n^2 float terms
// loses the drift signal being measured.
struct Potential {
    double u = 0;
};

struct Motion {
    double ke = 0, px = 0, py = 0, pz = 0;
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
