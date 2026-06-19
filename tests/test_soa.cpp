// Reflection + AoS->SoA storage tests.
#include "check.hpp"
#include "ecs/reflect.hpp"
#include "ecs/soa.hpp"

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

struct Tag {
};


static void reflection_counts() {
    static_assert(reflect::field_count_v<Vec3> == 3);
    static_assert(reflect::field_count_v<Transform> == 3);
    static_assert(reflect::field_count_v<Wide> == 18);
    // exercises 17..32 ladder
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

int main() {
    RUN_SUITE(reflection_counts);
    RUN_SUITE(scatter_gather);
    RUN_SUITE(column_is_contiguous_and_mutable);
    RUN_SUITE(swap_remove_keeps_columns_consistent);
    RUN_SUITE(tag_component_has_no_columns);
    return REPORT();
}