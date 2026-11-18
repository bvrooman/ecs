// Runtime (dynamic / JS-defined) component storage tests. Exercises the Phase A
// spine -- DynamicColumn + WorldOps -- directly in C++, independent of the wasm
// binding, so storage bugs surface natively in milliseconds.
#include <ecs/dynamic/native.hpp>
#include <ecs/dynamic/registry.hpp>
#include <ecs/dynamic/world_ops.hpp>
#include <ecs/schedule.hpp>
#include <ecs/world.hpp>

#include "check.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace ecs;
using namespace ecs::dynamic;

// A packed {x:f32, y:f32} blob -- the interchange format for a 2-float component.
struct F2 {
    float x, y;
};

// A native (compile-time, reflected) component -- the Phase C subject.
struct NPos {
    float x, y;
};

// Build the run-lambda for a dynamic system: for each archetype matching `query`,
// invoke kernel(count, archetype). This is the C++ stand-in for what the JS
// binding does per chunk (minus the typed-array views handed to a JS function).
template <class Kernel>
static auto each_chunk(Signature query, Kernel kernel) {
    // Signature normalizes (sorts + dedups) on construction.
    return [query  = std::move(query),
            kernel = std::move(kernel)](World& w, Commands&, detail::WorkItem const&) {
        for (auto const ai : w.matching_archetypes(query)) {
            auto& arch = dynamic::WorldOps::archetype_at(w, ai);
            if (arch.size() > 0)
                kernel(arch.size(), arch);
        }
    };
}

static float* base(Archetype& a, ComponentId id, std::size_t field) {
    return static_cast<float*>(
        static_cast<DynamicColumn&>(a.column_at(id)).field_base(field));
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
    constexpr int N          = 6;
    std::array<Entity, N> es = {Entity::null(),
                                Entity::null(),
                                Entity::null(),
                                Entity::null(),
                                Entity::null(),
                                Entity::null()};
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
    constexpr int N          = 4;
    std::array<Entity, N> es = {Entity::null(),
                                Entity::null(),
                                Entity::null(),
                                Entity::null()};
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

// A dynamic "gravity" then "integrate" pair, registered via add_dynamic_serial, must
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
    sched.add_dynamic_serial("gravity",
                             grav,
                             each_chunk({Vel}, [Vel](std::size_t n, Archetype& a) {
                                 auto* vy = base(a, Vel, 1);
                                 for (std::size_t i = 0; i < n; ++i)
                                     vy[i] += -10.f * 0.1f;
                             }));
    SystemAccess integ;
    integ.writes = {Pos};
    integ.reads  = {Vel};
    sched.add_dynamic_serial("integrate",
                             integ,
                             each_chunk({Pos, Vel},
                                        [Pos, Vel](std::size_t n, Archetype& a) {
                                            auto *px = base(a, Pos, 0),
                                                 *py = base(a, Pos, 1);
                                            auto *vx = base(a, Vel, 0),
                                                 *vy = base(a, Vel, 1);
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
    sched.add_dynamic_serial("integrate",
                             acc,
                             each_chunk({Pos, Vel},
                                        [Pos, Vel](std::size_t n, Archetype& a) {
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

// Phase C: a native (C++) component exposed to the runtime layer. register_native
// records its schema under component_id<NPos>; a runtime (Phase-B-style) system
// then reads & writes its SoA field through the type-erased IColumn::field_base,
// exactly as a JS system would. Entities are spawned natively (into Column<NPos>).
static void native_component_runtime_access() {
    auto const id = register_native<NPos>("NPos", {"x", "y"});
    CHECK(id == component_id<NPos>);
    CHECK(registry().desc(id).fields.size() == 2);
    CHECK(registry().desc(id).fields[0].type == FieldType::f32);
    CHECK(registry().desc(id).fields[0].name == "x");

    World w;
    Schedule sched;
    // Spawn the native entities in an earlier phase, so they exist (spawn
    // commands flush at the phase barrier) before the runtime system queries them.
    sched.add_serial(
        "spawn",
        [](Commands& cmd) {
            for (int i = 0; i < 4; ++i)
                cmd.spawn(NPos {float(i), 0.f});
        },
        phase {-1},
        times {1});
    SystemAccess acc;
    acc.writes = {component_id<NPos>};
    sched.add_dynamic_serial(
        "scale",
        acc,
        each_chunk({component_id<NPos>}, [](std::size_t n, Archetype& a) {
            // Field 0 of a *native* column, reached through the runtime
            // IColumn vtable.
            auto* xs = static_cast<float*>(a.column_at(component_id<NPos>).field_base(0));
            for (std::size_t i = 0; i < n; ++i)
                xs[i] *= 10.f;
        }));
    sched.run(w);

    CHECK(w.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK((w.get<NPos>(Entity::from_raw(std::uint32_t(i), 0)).x == float(i) * 10.f));
}

// A malformed spawn bundle is rejected when spawn_deferred records it, before a
// handle is reserved. A repeated component used to be accepted: the archetype got
// one column per distinct id but a row per entry, so that column ran a row longer
// than the archetype and later entities read their neighbour's values.
static void spawn_bundle_validation() {
    auto const Pos = define2("BundlePos");
    auto const Vel = define2("BundleVel");
    auto blob      = [](float x, float y) {
        std::vector<std::byte> b(sizeof(F2));
        F2 const v {x, y};
        std::memcpy(b.data(), &v, sizeof v);
        return b;
    };
    auto rejects = [](auto&& record) {
        try {
            record();
        } catch (std::invalid_argument const&) {
            return true;
        }
        return false;
    };

    World w;
    Entity const first = WorldOps::spawn_deferred(w, {{Pos, blob(1, 1)}});
    CHECK(rejects([&] { // Pos twice
        (void)WorldOps::spawn_deferred(w,
                                       {{Pos, blob(2, 2)},
                                        {Vel, blob(0, 0)},
                                        {Pos, blob(9, 9)}});
    }));
    CHECK(rejects([&] { // 4 bytes for an 8-byte component
        (void)WorldOps::spawn_deferred(w, {{Pos, std::vector<std::byte>(4)}});
    }));
    CHECK(rejects([&] { // a native component's id
        (void)WorldOps::spawn_deferred(w, {{component_id<NPos>, blob(0, 0)}});
    }));
    // The rejected calls reserved nothing: the next spawn takes the next index.
    Entity const second =
        WorldOps::spawn_deferred(w, {{Pos, blob(3, 3)}, {Vel, blob(4, 4)}});
    CHECK(second.index() == first.index() + 1);

    // Both valid spawns apply at the flush; the rejections did not poison it.
    Schedule flush;
    flush.add_serial("flush", [] {});
    flush.run(w);
    CHECK(w.size() == 2);
    F2 out {};
    CHECK(WorldOps::get(w, first, Pos, &out));
    CHECK(out.x == 1.f && out.y == 1.f);
    CHECK(WorldOps::get(w, second, Pos, &out));
    CHECK(out.x == 3.f && out.y == 3.f);
    CHECK(WorldOps::get(w, second, Vel, &out));
    CHECK(out.x == 4.f && out.y == 4.f);

    // build_now, the immediate form, rejects the same bundle before it touches
    // the world (the handle is never reached, so any value will do).
    CHECK(rejects([&] {
        WorldOps::build_now(w,
                            Entity::from_raw(99, 0),
                            {{Vel, blob(5, 5)}, {Vel, blob(6, 6)}});
    }));
    CHECK(w.size() == 2);
    for (auto const& arch : std::as_const(w).archetypes())
        for (auto const& col : arch->columns)
            CHECK(col->size() == arch->entities.size());
}

// A packed F2 bundle entry for spawn_deferred.
static std::vector<std::byte> f2_blob(float x, float y) {
    std::vector<std::byte> b(sizeof(F2));
    F2 const v {x, y};
    std::memcpy(b.data(), &v, sizeof v);
    return b;
}

// Field 0 of `id` across every archetype holding it, in walk order.
static std::vector<float> gather_x(World& w, ComponentId id) {
    std::vector<float> out;
    for (auto const ai : w.matching_archetypes(Signature({id}))) {
        auto& a        = WorldOps::archetype_at(w, ai);
        auto const* xs = base(a, id, 0);
        out.insert(out.end(), xs, xs + a.size());
    }
    return out;
}

struct Spawned {
    int v = 0;
};

// Dynamic systems' commands replay in canonical order, both the ones recorded
// through the Commands& their run is handed and the ones recorded through
// WorldOps (what a JS kernel's DynamicWorld.spawn reaches) while the system
// runs: each work item records into its own store, drained at the flush in
// wave order (registration order within a wave), a parallel system's items in
// ordinal order. So the spawned rows come out in exactly that order at any
// lane count -- through the thread-sharded buffer they followed lane timing.
static void dynamic_commands_replay_in_canonical_order() {
    auto const Pos      = define2("CanonPos");
    auto const Mark     = define2("CanonMark");
    constexpr int kRows = 12'000;
    constexpr int kPer  = 100;

    auto emitter = [Mark](int const k) { // dynamic serial: WorldOps only
        return [Mark, k](World& w, Commands&, detail::WorkItem const&) {
            for (int j = 0; j < kPer; ++j)
                (void)WorldOps::spawn_deferred(w,
                                               {{Mark,
                                                 f2_blob(float(k * 1'000 + j), 0)}});
        };
    };
    auto run_and_gather = [&](unsigned lanes) {
        World w;
        for (int i = 0; i < kRows; ++i) {
            Entity const e = WorldOps::create_entity(w);
            F2 const v {float(i), 0};
            WorldOps::add(w, e, Pos, &v);
        }
        SystemAccess emits;
        emits.commands = true;
        SystemAccess walks;
        walks.reads    = {Pos};
        walks.commands = true;

        Schedule s;
        s.add_dynamic_serial("e0", emits, emitter(0));
        s.add_dynamic_serial("e1", emits, emitter(1));
        // Dynamic parallel: both recording paths, from every lane.
        s.add_dynamic_parallel(
            "walk",
            walks,
            Signature({Pos}),
            [Pos, Mark](World& w, Commands& cmd, detail::WorkItem const& item) {
                for (auto const& u : item.units) {
                    auto const* xs = base(WorldOps::archetype_at(w, u.archetype), Pos, 0);
                    for (auto r = u.begin; r < u.end; ++r) {
                        int const x = int(xs[r]);
                        if (x % 7 != 0)
                            continue;
                        cmd.spawn(Spawned {x});
                        (void)WorldOps::spawn_deferred(w,
                                                       {{Mark,
                                                         f2_blob(float(-(x + 1)), 0)}});
                    }
                }
            });
        s.add_dynamic_serial("e2", emits, emitter(2));
        s.add_dynamic_serial("e3", emits, emitter(3));
        CHECK(s.level_count() == 1); // no access conflict -> one shared wave

        WorkerPool pool {lanes};
        for (int t = 0; t < 2; ++t) // second tick: stores rewound and reused
            s.run(w, pool);
        std::vector<int> spawned;
        w.for_each<Spawned const>([&](auto& sp) { spawned.push_back(sp.v); });
        return std::pair {gather_x(w, Mark), spawned};
    };

    // Per tick: e0, e1, then walk's rows in serial-walk order, then e2, e3.
    std::vector<float> marks;
    std::vector<int> spawns;
    for (int t = 0; t < 2; ++t) {
        for (int k = 0; k < 2; ++k)
            for (int j = 0; j < kPer; ++j)
                marks.push_back(float(k * 1'000 + j));
        for (int x = 0; x < kRows; x += 7) {
            marks.push_back(float(-(x + 1)));
            spawns.push_back(x);
        }
        for (int k = 2; k < 4; ++k)
            for (int j = 0; j < kPer; ++j)
                marks.push_back(float(k * 1'000 + j));
    }
    for (unsigned lanes : {1u, 2u, 4u}) {
        auto const [got_marks, got_spawns] = run_and_gather(lanes);
        CHECK(got_marks == marks);   // WorldOps edits: wave and ordinal order
        CHECK(got_spawns == spawns); // Commands& edits: ordinal order
    }
}

// WorldOps recording into a DIFFERENT world from inside a system must not be
// captured by the running item's store (which flushes into the world the
// schedule runs on): it goes to the target world's own buffer and applies at
// that world's next flush.
static void worldops_into_another_world_stays_there() {
    auto const Mark = define2("ElsewhereMark");
    World a, b;
    SystemAccess emits;
    emits.commands = true;
    Schedule s;
    s.add_dynamic_serial(
        "emit",
        emits,
        [&b, Mark](World& w, Commands&, detail::WorkItem const&) {
            (void)WorldOps::spawn_deferred(w, {{Mark, f2_blob(1, 0)}}); // own world
            (void)WorldOps::spawn_deferred(b, {{Mark, f2_blob(2, 0)}}); // another
        });
    s.run(a);
    CHECK(gather_x(a, Mark) == std::vector<float> {1}); // only its own spawn
    CHECK(b.size() == 0);                               // b's waits for b's flush

    Schedule flush;
    flush.add_serial("flush", [] {});
    flush.run(b);
    CHECK(gather_x(b, Mark) == std::vector<float> {2});
    CHECK(gather_x(a, Mark) == std::vector<float> {1});
}

// A dynamic system that throws aborts the run: neither its Commands& edits nor
// its WorldOps edits apply, and they do not resurface on the next run.
static void dynamic_commands_discarded_on_abort() {
    auto const Mark = define2("AbortMark");
    World w;
    bool fail = true;
    SystemAccess emits;
    emits.commands = true;
    Schedule s;
    s.add_dynamic_serial("emit",
                         emits,
                         [&fail, Mark](World& w, Commands& cmd, detail::WorkItem const&) {
                             cmd.spawn(Spawned {1});
                             (void)WorldOps::spawn_deferred(w, {{Mark, f2_blob(1, 0)}});
                             if (fail)
                                 throw std::runtime_error("boom");
                         });
    bool threw = false;
    try {
        s.run(w);
    } catch (std::runtime_error const&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(w.size() == 0); // nothing applied

    fail = false;
    s.run(w);
    CHECK(w.size() == 2); // this run's two spawns, not the aborted run's too
    CHECK(w.count<Spawned const>() == 1);
    CHECK(gather_x(w, Mark) == std::vector<float> {1});
}

// A WorldOps::spawn_deferred recorded by a system whose run aborts never
// applies: its handle stays dead and its slot is handed back one generation on,
// exactly like a Commands::spawn.
static void aborted_deferred_spawn_is_released() {
    auto const Pos = define2("AbortedSpawnPos");
    auto blob      = [] {
        F2 const v {1, 2};
        std::vector<std::byte> b(sizeof v);
        std::memcpy(b.data(), &v, sizeof v);
        return b;
    };
    World w;
    Entity e = Entity::null();
    Schedule s;
    s.add_serial("spawn_then_throw", [&] {
        e = WorldOps::spawn_deferred(w, {{Pos, blob()}});
        throw std::runtime_error("abort");
    });
    bool threw = false;
    try {
        s.run(w);
    } catch (std::runtime_error const&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(w.size() == 0);
    CHECK(!w.alive(e));

    Entity const again = WorldOps::spawn_deferred(w, {{Pos, blob()}});
    CHECK(again.index() == e.index());
    CHECK(again.generation() == e.generation() + 1);
}

int main() {
    RUN_SUITE(round_trip);
    RUN_SUITE(archetype_transitions);
    RUN_SUITE(multi_entity_and_view);
    RUN_SUITE(destroy_keeps_storage_consistent);
    RUN_SUITE(dynamic_systems_run_and_level);
    RUN_SUITE(dynamic_system_filters_by_archetype);
    RUN_SUITE(native_component_runtime_access);
    RUN_SUITE(spawn_bundle_validation);
    RUN_SUITE(dynamic_commands_replay_in_canonical_order);
    RUN_SUITE(worldops_into_another_world_stays_there);
    RUN_SUITE(dynamic_commands_discarded_on_abort);
    RUN_SUITE(aborted_deferred_spawn_is_released);
    return REPORT();
}
