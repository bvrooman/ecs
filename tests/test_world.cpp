// World / archetype / query tests.
#include "check.hpp"
#include "setup.hpp"
#include <string>
#include <utility>
#include <vector>

using namespace ecs;

struct Position {
    float x, y;
};
struct Velocity {
    float dx, dy;
};
struct Frozen {}; // tag

static void create_and_query() {
    World w;
    Entity a = Entity::null(), b = Entity::null(), c = Entity::null();
    setup(w, [&](Commands& cmd) {
        a = cmd.spawn(Position {0, 0}, Velocity {1, 2});
        b = cmd.spawn(Position {10, 10});
        c = cmd.spawn(Position {5, 5}, Velocity {-1, -1}, Frozen {});
    });
    CHECK(w.size() == 3);
    CHECK(w.has<Velocity>(a));
    CHECK(!w.has<Velocity>(b));
    CHECK(w.has<Frozen>(c));
    CHECK((w.count<Position, Velocity>() == 2));

    // Value mutation runs through a real system's Query -- Position is the
    // declared write, Velocity a read. (Ad-hoc World iteration is read-only.)
    run_system(w, [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.dx;
            p.y += v.dy;
        });
    });
    CHECK(w.get<Position>(a).x == 1.f);
    CHECK(w.get<Position>(c).x == 4.f);
    CHECK(w.get<Position>(b).x == 10.f); // no Velocity -> untouched
}

static void add_remove_moves_archetype_preserving_data() {
    World w;
    Entity b = Entity::null();
    setup(w, [&](Commands& cmd) { b = cmd.spawn(Position {10, 20}); });
    CHECK((w.count<Position, Velocity>() == 0));

    setup(w, [&](Commands& cmd) { cmd.add<Velocity>(b, Velocity {3, 3}); });
    CHECK(w.has<Velocity>(b));
    CHECK(w.get<Position>(b).y == 20.f); // Position survived the move
    CHECK((w.count<Position, Velocity>() == 1));

    setup(w, [&](Commands& cmd) { cmd.remove<Velocity>(b); });
    CHECK(!w.has<Velocity>(b));
    CHECK(w.get<Position>(b).x == 10.f);
    CHECK((w.count<Position, Velocity>() == 0));
}

static void destroy_and_generation_reuse() {
    World w;
    Entity a = Entity::null(), a2 = Entity::null();
    setup(w, [&](Commands& cmd) {
        a  = cmd.spawn(Position {1, 1});
        a2 = cmd.spawn(Position {2, 2});
    });
    (void)a2;
    Entity old = a;
    setup(w, [&](Commands& cmd) { cmd.destroy(a); });
    CHECK(!w.alive(old));
    CHECK(w.size() == 1);

    Entity d = Entity::null();
    setup(w, [&](Commands& cmd) { d = cmd.spawn(); });
    CHECK(d.index() == old.index());           // slot reused
    CHECK(d.generation() != old.generation()); // stale handle won't match
    CHECK(!w.alive(old));
}

static void soa_fast_path() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 4; ++i)
            cmd.spawn(Position {float(i), 0});
    });
    run_system(w, [](Query<Position> q) {
        q.for_each_chunk([](std::span<Entity const>, chunk<Position> pos) {
            for (auto& x : pos.column<0>())
                x += 100.f;
        });
    });
    // every Position.x was bumped by 100 via a contiguous column loop
    std::size_t seen = 0;
    w.for_each<Position>([&](auto& p) {
        CHECK(p.x >= 100.f);
        ++seen;
    });
    CHECK(seen == 4);
}

#if ECS_USE_P2996
// Per-element SoA iteration via row proxies (P2996-only: needs field names).
static void for_each_rows() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 4; ++i)
            cmd.spawn(Position {float(i), 0.f}, Velocity {1.f, 2.f});
    });
    // A mutating system: named proxy fields write through, no manual loop.
    run_system(w, [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.dx; // +1
            p.y += v.dy; // +2
        });
    });
    // for_each: same ergonomics, serial path (read here).
    std::size_t seen = 0;
    w.for_each<Position const>([&](auto& p) {
        CHECK(p.y == 2.f); // 0 + dy
        ++seen;
    });
    CHECK(seen == 4);
    // entity form: a leading Entity parameter is detected and passed, both methods.
    std::size_t es = 0, ep = 0;
    w.for_each<Position const>([&](Entity e, auto& p) {
        CHECK(w.alive(e));
        (void)p;
        ++es;
    });
    w.for_each<Position const>([&](Entity e, auto& p) {
        CHECK(w.alive(e));
        (void)p;
        ++ep;
    });
    CHECK(es == 4);
    CHECK(ep == 4);
}
#endif

static void const_query_marks_read_only() {
    World w;
    Entity e = Entity::null();
    setup(w, [&](Commands& cmd) { e = cmd.spawn(Position {1, 1}, Velocity {2, 3}); });
    // Velocity read-only (const), Position the declared write.
    run_system(w, [](Query<Velocity const, Position> q) {
        q.for_each([](auto& v, auto& p) {
            p.x += v.dx;
        });
    });
    CHECK(w.get<Position>(e).x == 3.f);  // 1 + 2
    CHECK(w.get<Velocity>(e).dx == 2.f); // untouched

    // for_each_chunk hands a const storage for the read-only component. (Note the
    // query also matches the now-empty intermediate {Position} archetype left by
    // the incremental spawn, so accumulate across chunks.)
    float sum_x      = 0;
    std::size_t seen = 0;
    w.for_each_chunk<Position const>([&](std::span<Entity const>,
                                                chunk<Position const> pos) {
        for (float x : pos.column<0>()) { // span<const float>
            sum_x += x;
            ++seen;
        }
    });
    CHECK(seen == 1);
    CHECK(sum_x == 3.f);
}

// A serial system's Query spans every matching archetype -- including ones left
// empty by migration -- so Query::for_each_chunk must skip empty chunks (never
// hand the kernel a zero-length column span).
static void query_chunk_skips_empty_archetype() {
    World w;
    Entity e = Entity::null();
    setup(w, [&](Commands& cmd) { e = cmd.spawn(Position {1, 1}); }); // -> {Position}
    setup(w, [&](Commands& cmd) {
        cmd.add<Velocity>(e, Velocity {2, 2}); // migrate -> {Position,Velocity}, {Position} now empty
    });
    // Query<Position> matches BOTH the emptied {Position} and {Position,Velocity}.
    std::size_t invocations = 0, rows = 0;
    run_system(w, [&](Query<Position const> q) {
        q.for_each_chunk([&](std::span<Entity const> ents, chunk<Position const> pos) {
            ++invocations;
            rows += pos.column<0>().size();
            CHECK(pos.column<0>().size() > 0); // never an empty chunk
            CHECK(ents.size() > 0);
        });
    });
    CHECK(invocations == 1); // only the non-empty archetype
    CHECK(rows == 1);
}

static void spawn_goes_directly_to_final_archetype() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 100; ++i)
            cmd.spawn(Position {float(i), 0}, Velocity {1, 1});
    });
    CHECK(w.size() == 100);
    CHECK((w.count<Position, Velocity>() == 100));
    // Only the empty archetype and {Position, Velocity} exist -- spawning does not
    // pass through (and leave behind) an empty intermediate {Position} archetype.
    CHECK(std::as_const(w).archetypes().size() == 2);
}

static void query_cache_sees_archetypes_created_after_first_query() {
    World w;
    setup(w, [&](Commands& cmd) { cmd.spawn(Position {1, 1}); }); // {Position}
    CHECK((w.count<Position>() == 1)); // builds the cache for {Position}

    // A new archetype {Position, Velocity} created later must enter the cache.
    setup(w, [&](Commands& cmd) { cmd.spawn(Position {2, 2}, Velocity {1, 1}); });
    CHECK((w.count<Position>() == 2));
    CHECK((w.count<Position, Velocity>() == 1));
}

// Repeated add/remove of the same component cycles an entity between two
// archetypes through the (cached-after-first-use) edge path; values must
// survive every hop and the row bookkeeping must stay exact.
static void add_remove_churn_preserves_data() {
    World w;
    Entity a = Entity::null(), b = Entity::null();
    setup(w, [&](Commands& cmd) {
        a = cmd.spawn(Position {1, 2});
        b = cmd.spawn(Position {3, 4});
    });

    Schedule s;
    auto churn = s.add("churn", [&](Query<const Position>, Commands& cmd) {
        if (w.has<Velocity>(a))
            cmd.remove<Velocity>(a);
        else
            cmd.add(a, Velocity {9, 9});
    });
    for (int i = 0; i < 20; ++i)
        s.run(w);
    s.remove(churn);

    CHECK(w.get<Position>(a).x == 1.f);
    CHECK(w.get<Position>(b).x == 3.f);
    CHECK(!w.has<Velocity>(a)); // 20 toggles: back off
    CHECK(w.size() == 2);
}

// Components with heap-owning fields must survive spawn, archetype hops, and
// swap-remove intact (exercises the move-based relocation path; run under
// ASAN this also proves no leak/double-free).
struct Label {
    std::string text;
    std::vector<int> data;
};
static void heap_owning_component_survives_transitions() {
    World w;
    Entity keep = Entity::null(), dead = Entity::null();
    setup(w, [&](Commands& cmd) {
        keep = cmd.spawn(Position {0, 0},
                         Label {"the quick brown fox jumps over strings' SSO",
                                {1, 2, 3, 4, 5}});
        dead = cmd.spawn(Position {0, 0}, Label {"short", {6}});
    });
    setup(w, [&](Commands& cmd) {
        cmd.add(keep, Velocity {1, 1}); // relocate {P,L} -> {P,L,V}
        cmd.destroy(dead);              // swap-remove in the old archetype
    });
    CHECK(w.get<Label>(keep).text ==
          "the quick brown fox jumps over strings' SSO");
    CHECK(w.get<Label>(keep).data.size() == 5);
    CHECK(!w.alive(dead));
}

static void entity_null_sentinel() {
    CHECK(!Entity::null());
    CHECK(static_cast<bool>(Entity::from_raw(0, 0))); // the FIRST entity is not null
    CHECK(Entity::null() == Entity::null());
    World w;
    CHECK(!w.alive(Entity::null()));
    // operator<=> so handles sort / work in ordered containers.
    CHECK((Entity::from_raw(0, 0) < Entity::from_raw(1, 0)));
}

int main() {
    RUN_SUITE(create_and_query);
    RUN_SUITE(add_remove_moves_archetype_preserving_data);
    RUN_SUITE(destroy_and_generation_reuse);
    RUN_SUITE(soa_fast_path);
#if ECS_USE_P2996
    RUN_SUITE(for_each_rows);
#endif
    RUN_SUITE(const_query_marks_read_only);
    RUN_SUITE(query_chunk_skips_empty_archetype);
    RUN_SUITE(spawn_goes_directly_to_final_archetype);
    RUN_SUITE(query_cache_sees_archetypes_created_after_first_query);
    RUN_SUITE(add_remove_churn_preserves_data);
    RUN_SUITE(heap_owning_component_survives_transitions);
    RUN_SUITE(entity_null_sentinel);
    return REPORT();
}
