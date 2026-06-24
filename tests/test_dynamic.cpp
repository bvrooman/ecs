// Runtime (dynamic / JS-defined) component storage tests. Exercises the Phase A
// spine -- DynamicColumn + WorldOps -- directly in C++, independent of the wasm
// binding, so storage bugs surface natively in milliseconds.
#include "check.hpp"
#include "ecs/dynamic/registry.hpp"
#include "ecs/dynamic/world_ops.hpp"
#include "ecs/schedule.hpp"
#include "ecs/world.hpp"

#include <algorithm>

using namespace ecs;
using namespace ecs::dynamic;

// A packed {x:f32, y:f32} blob -- the interchange format for a 2-float component.
struct F2 {
    float x, y;
};

// Build the run-lambda for a dynamic system: for each archetype matching `query`,
// invoke kernel(count, archetype). This is the C++ stand-in for what the JS
// binding does per chunk (minus the typed-array views handed to a JS function).
template <class Kernel>
static auto each_chunk(Signature query, Kernel kernel) {
    std::ranges::sort(query);
    return [query = std::move(query), kernel = std::move(kernel)](
               World& w, Commands&, WorkerPool*) {
        for (auto const ai : w.matching_archetypes(query)) {
            auto& arch = *w.archetypes()[ai];
            if (arch.size() > 0)
                kernel(arch.size(), arch);
        }
    };
}

static float* base(Archetype& a, ComponentId id, std::size_t field) {
    return static_cast<float*>(
        static_cast<DynamicColumn&>(*a.columns.at(id)).field_base(field));
}

static ComponentId define2(char const* name) {
    return registry().define(name, {{"x", FieldType::f32}, {"y", FieldType::f32}});
}

static void round_trip() {
    auto const Pos = define2("Position");
    World w;
    Entity const e = WorldOps::create_entity(w);
    CHECK(w.size() == 1);

    F2 p {1.5f, -2.5f};
    WorldOps::add(w, e, Pos, &p);
    CHECK(WorldOps::has(w, e, Pos));

    F2 got {};
    CHECK(WorldOps::get(w, e, Pos, &got));
    CHECK(got.x == 1.5f);
    CHECK(got.y == -2.5f);

    F2 np {9.f, 9.f};
    CHECK(WorldOps::set(w, e, Pos, &np));
    WorldOps::get(w, e, Pos, &got);
    CHECK(got.x == 9.f);
    CHECK(got.y == 9.f);
}

static void archetype_transitions() {
    auto const Pos = define2("PositionB");
    auto const Vel = define2("VelocityB");
    World w;
    Entity const e = WorldOps::create_entity(w);

    F2 p {1, 2}, v {3, 4};
    WorldOps::add(w, e, Pos, &p);
    WorldOps::add(w, e, Vel, &v); // entity now lives in archetype {Pos, Vel}
    CHECK(WorldOps::has(w, e, Pos));
    CHECK(WorldOps::has(w, e, Vel));

    WorldOps::remove(w, e, Vel); // moves back to archetype {Pos}, data preserved
    CHECK(!WorldOps::has(w, e, Vel));
    CHECK(WorldOps::has(w, e, Pos));
    F2 got {};
    WorldOps::get(w, e, Pos, &got);
    CHECK(got.x == 1.f); // Position survived the relocation
    CHECK(got.y == 2.f);
}

static void multi_entity_and_view() {
    auto const Pos = define2("PositionC");
    World w;
    constexpr int N = 6;
    Entity es[N];
    for (int i = 0; i < N; ++i) {
        es[i] = WorldOps::create_entity(w);
        F2 p {float(i), float(i * 2)};
        WorldOps::add(w, es[i], Pos, &p); // all land in the single {Pos} archetype
    }
    CHECK(w.size() == N);

    // The SoA payoff: field 0 (x) is one contiguous run across all N entities,
    // in insertion order. Writing through it must be visible via get().
    auto* column = WorldOps::column(w, Pos);
    CHECK(column != nullptr);
    CHECK(column->size() == N);
    auto* xs = static_cast<float*>(column->field_base(0));
    for (int i = 0; i < N; ++i)
        CHECK(xs[i] == float(i)); // matches what add() scattered

    for (int i = 0; i < N; ++i)
        xs[i] = float(i * 10); // mutate through the raw field view
    for (int i = 0; i < N; ++i) {
        F2 got {};
        WorldOps::get(w, es[i], Pos, &got);
        CHECK(got.x == float(i * 10)); // aliasing confirmed
    }
}

static void destroy_keeps_storage_consistent() {
    auto const Pos = define2("PositionD");
    World w;
    constexpr int N = 4;
    Entity es[N];
    for (int i = 0; i < N; ++i) {
        es[i] = WorldOps::create_entity(w);
        F2 p {float(i), 0};
        WorldOps::add(w, es[i], Pos, &p);
    }
    WorldOps::destroy(w, es[1]); // swap-remove: last row (es[3]) moves into row 1
    CHECK(w.size() == N - 1);
    CHECK(!WorldOps::has(w, es[1], Pos));

    // The surviving entities still read their own values (record rows patched).
    F2 got {};
    WorldOps::get(w, es[0], Pos, &got);
    CHECK(got.x == 0.f);
    WorldOps::get(w, es[3], Pos, &got);
    CHECK(got.x == 3.f);
    WorldOps::get(w, es[2], Pos, &got);
    CHECK(got.x == 2.f);
}

// A dynamic "gravity" then "integrate" pair, registered via add_dynamic, must
// (a) level correctly off their declared access -- integrate reads Velocity that
// gravity writes, so it runs second -- and (b) mutate storage through the column
// views. dt=0.1, g=-10 over one tick from Pos{0,0}, Vel{1,0}:
//   gravity:   Vel.y += g*dt = -1
//   integrate: Pos += Vel*dt = (0.1, -0.1)   <- uses the *post-gravity* Vel
static void dynamic_systems_run_and_level() {
    auto const Pos = define2("PositionE");
    auto const Vel = define2("VelocityE");
    World w;
    Entity const e = WorldOps::create_entity(w);
    F2 p {0, 0}, v {1, 0};
    WorldOps::add(w, e, Pos, &p);
    WorldOps::add(w, e, Vel, &v);

    Schedule sched;
    SystemAccess grav;
    grav.writes = {Vel};
    sched.add_dynamic("gravity", grav, each_chunk({Vel}, [Vel](std::size_t n, Archetype& a) {
        auto* vy = base(a, Vel, 1);
        for (std::size_t i = 0; i < n; ++i)
            vy[i] += -10.f * 0.1f;
    }));
    SystemAccess integ;
    integ.writes = {Pos};
    integ.reads  = {Vel};
    sched.add_dynamic("integrate", integ, each_chunk({Pos, Vel}, [Pos, Vel](std::size_t n, Archetype& a) {
        auto *px = base(a, Pos, 0), *py = base(a, Pos, 1);
        auto *vx = base(a, Vel, 0), *vy = base(a, Vel, 1);
        for (std::size_t i = 0; i < n; ++i) {
            px[i] += vx[i] * 0.1f;
            py[i] += vy[i] * 0.1f;
        }
    }));

    // gravity (writes Vel) and integrate (reads Vel) conflict -> two waves.
    CHECK(sched.level_count() == 2);

    sched.run(w);
    F2 gp {}, gv {};
    WorldOps::get(w, e, Pos, &gp);
    WorldOps::get(w, e, Vel, &gv);
    CHECK(gv.y == -1.f);  // gravity applied
    CHECK(gp.x == 0.1f);  // integrate used post-gravity velocity...
    CHECK(gp.y == -0.1f); // ...so y moved (ordering is correct)
}

// A dynamic system queries entities by component set: an entity missing one of
// the queried components must be skipped (archetype filtering).
static void dynamic_system_filters_by_archetype() {
    auto const Pos = define2("PositionF");
    auto const Vel = define2("VelocityF");
    World w;
    Entity const moving = WorldOps::create_entity(w); // has Pos + Vel
    Entity const fixed  = WorldOps::create_entity(w); // has Pos only
    F2 zero {0, 0}, vel {5, 0};
    WorldOps::add(w, moving, Pos, &zero);
    WorldOps::add(w, moving, Vel, &vel);
    WorldOps::add(w, fixed, Pos, &zero);

    Schedule sched;
    SystemAccess acc;
    acc.writes = {Pos};
    acc.reads  = {Vel};
    sched.add_dynamic("integrate", acc, each_chunk({Pos, Vel}, [Pos, Vel](std::size_t n, Archetype& a) {
        auto* px = base(a, Pos, 0);
        auto* vx = base(a, Vel, 0);
        for (std::size_t i = 0; i < n; ++i)
            px[i] += vx[i] * 0.1f;
    }));
    sched.run(w);

    F2 mp {}, fp {};
    WorldOps::get(w, moving, Pos, &mp);
    WorldOps::get(w, fixed, Pos, &fp);
    CHECK(mp.x == 0.5f); // moving entity integrated (5 * 0.1)
    CHECK(fp.x == 0.f);  // Pos-only entity untouched (didn't match the query)
}

int main() {
    RUN_SUITE(round_trip);
    RUN_SUITE(archetype_transitions);
    RUN_SUITE(multi_entity_and_view);
    RUN_SUITE(destroy_keeps_storage_consistent);
    RUN_SUITE(dynamic_systems_run_and_level);
    RUN_SUITE(dynamic_system_filters_by_archetype);
    return REPORT();
}
