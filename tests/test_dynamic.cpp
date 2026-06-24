// Runtime (dynamic / JS-defined) component storage tests. Exercises the Phase A
// spine -- DynamicColumn + WorldOps -- directly in C++, independent of the wasm
// binding, so storage bugs surface natively in milliseconds.
#include "check.hpp"
#include "ecs/dynamic/registry.hpp"
#include "ecs/dynamic/world_ops.hpp"
#include "ecs/world.hpp"

using namespace ecs;
using namespace ecs::dynamic;

// A packed {x:f32, y:f32} blob -- the interchange format for a 2-float component.
struct F2 {
    float x, y;
};

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

int main() {
    RUN_SUITE(round_trip);
    RUN_SUITE(archetype_transitions);
    RUN_SUITE(multi_entity_and_view);
    RUN_SUITE(destroy_keeps_storage_consistent);
    return REPORT();
}
