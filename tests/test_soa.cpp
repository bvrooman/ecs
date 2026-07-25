// Reflection + AoS->SoA storage tests.
#include "check.hpp"
#include "ecs/reflection/reflect.hpp"
#include "ecs/soa.hpp"
#include <stdexcept>

using namespace ecs;

struct Vec3 {
    float x, y, z;
};

struct Transform {
    Vec3 pos;
    float mass;
    int id;
};

struct Wide {
    int a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p, q, r;
};

// 32 fields: the upper bound of the portable backend's field_tie ladder.
struct Max32 {
    int v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12, v13, v14, v15, v16, v17,
        v18, v19, v20, v21, v22, v23, v24, v25, v26, v27, v28, v29, v30, v31;
};

struct Tag {};

static void reflection_counts() {
    static_assert(reflect::field_count_v<Vec3> == 3);
    static_assert(reflect::field_count_v<Transform> == 3);
    static_assert(reflect::field_count_v<Wide> == 18);
    static_assert(reflect::field_count_v<Max32> == 32);
    static_assert(reflect::field_count_v<Tag> == 0);
    CHECK((reflect::field_count_v<Transform> == 3));
}

static void scatter_gather() {
    soa_storage<Transform> s;
    s.push_back({{1, 2, 3}, 10.f, 7});
    s.push_back({{4, 5, 6}, 20.f, 8});
    CHECK(s.size() == 2);

    Transform t = s.gather(1);
    CHECK(t.pos.y == 5.f);
    CHECK(t.mass == 20.f);
    CHECK(t.id == 8);
}

// Components with 17..32 fields exercise the upper half of the field_tie ladder
// (push_back scatters and gather reassembles, both via get_field). Regression:
// the 17..31 cases used to bind 32 names regardless of N and failed to compile;
// only field_count_v -- which counts via the brace-arity probe, not field_tie --
// had ever touched a wide struct.
static void wide_scatter_gather() {
    soa_storage<Wide> w; // 18 fields (in the previously-broken 17..31 range)
    w.push_back({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18});
    CHECK(w.size() == 1);
    Wide g = w.gather(0);
    CHECK(g.a == 1);                // first field round-trips
    CHECK(g.r == 18);               // 18th field round-trips
    CHECK(w.column<17>()[0] == 18); // last column is the scattered field

    soa_storage<Max32> m; // 32 fields (the ladder's upper bound)
    m.push_back({0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31});
    CHECK(m.gather(0).v0 == 0);
    CHECK(m.gather(0).v31 == 31);
    CHECK(m.column<31>()[0] == 31);
}

static void column_is_contiguous_and_mutable() {
    soa_storage<Transform> s;
    s.push_back({{1, 2, 3}, 10.f, 7});
    s.push_back({{4, 5, 6}, 20.f, 8});

    auto masses = s.column<1>(); // the SoA payoff: one field, contiguous
    CHECK(masses.size() == 2);
    CHECK(masses[0] == 10.f);
    CHECK(masses[1] == 20.f);
    CHECK(&masses[1] - &masses[0] == 1); // truly adjacent in memory

    masses[0] = 99.f;
    CHECK(s.gather(0).mass == 99.f);
}

static void swap_remove_keeps_columns_consistent() {
    soa_storage<Transform> s;
    s.push_back({{1, 1, 1}, 1.f, 1});
    s.push_back({{2, 2, 2}, 2.f, 2});
    s.push_back({{3, 3, 3}, 3.f, 3});
    s.swap_remove(0); // last (id 3) moves into slot 0
    CHECK(s.size() == 2);
    CHECK(s.gather(0).id == 3);
    CHECK(s.gather(1).id == 2);
}

static void tag_component_has_no_columns() {
    soa_storage<Tag> tags;
    tags.push_back({});
    tags.push_back({});
    CHECK(tags.size() == 2);
    CHECK(soa_storage<Tag>::field_count == 0);
}

// A field type whose copy constructor throws on demand -- used to prove
// push_back is transactional (a mid-scatter throw must not leave the columns
// at different lengths, which would silently corrupt every later row).
struct Fickle {
    static inline bool arm = false;
    int v                  = 0;
    Fickle()               = default;
    explicit Fickle(int x)
        : v(x) {}
    Fickle(Fickle const& o)
        : v(o.v) {
        if (arm)
            throw std::runtime_error("fickle copy");
    }
    Fickle& operator=(Fickle const&) = default;
    Fickle(Fickle&&)                 = default;
    Fickle& operator=(Fickle&&)      = default;
};
struct TwoField {
    int a = 0; // scatters first, succeeds
    Fickle b;  // scatters second, may throw
};

static void push_back_is_transactional_on_throw() {
    soa_storage<TwoField> s;
    s.push_back(TwoField {1, Fickle {10}});
    CHECK(s.size() == 1);

    Fickle::arm = true;
    bool threw  = false;
    TwoField const bomb {2, Fickle {20}};
    try {
        s.push_back(bomb); // copy of .b throws AFTER .a was appended
    } catch (std::runtime_error const&) {
        threw = true;
    }
    Fickle::arm = false;
    CHECK(threw);

    // The failed push must have rolled back completely: still exactly one row,
    // both columns agreeing on it.
    CHECK(s.size() == 1);
    CHECK(s.column<0>().size() == 1);
    CHECK(s.column<1>().size() == 1);
    CHECK(s.gather(0).a == 1);
    CHECK(s.gather(0).b.v == 10);

    // And the storage remains fully usable afterwards.
    s.push_back(TwoField {3, Fickle {30}});
    CHECK(s.size() == 2);
    CHECK(s.gather(1).a == 3);
}

int main() {
    RUN_SUITE(reflection_counts);
    RUN_SUITE(scatter_gather);
    RUN_SUITE(wide_scatter_gather);
    RUN_SUITE(column_is_contiguous_and_mutable);
    RUN_SUITE(swap_remove_keeps_columns_consistent);
    RUN_SUITE(tag_component_has_no_columns);
    RUN_SUITE(push_back_is_transactional_on_throw);
    return REPORT();
}