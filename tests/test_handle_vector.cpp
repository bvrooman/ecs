#include "check.hpp"
#include "ecs/detail/handle_vector.hpp"

#include <string>

using ecs::detail::HandleVector;

static void test_create_returns_valid_handle() {
    auto i = HandleVector<std::string> {};
    auto h = i.create("hello");
    CHECK(h.index() == 0);
    CHECK(h.generation() == 0);
}

static void test_get_returns_value_for_valid_handle() {
    auto i   = HandleVector<std::string> {};
    auto h   = i.create("hello");
    auto* get = i.get(h);
    CHECK(get != nullptr && *get == "hello");
}

static void test_get_returns_none_for_dead_handle() {
    auto i  = HandleVector<std::string> {};
    auto h0 = i.create("hello");
    i.destroy(h0);
    (void)i.create("hello");
    auto* get = i.get(h0);
    CHECK(get == nullptr);
}

static void test_create_destroy_create_reuses_slot() {
    auto i = HandleVector<std::string> {};
    {
        auto const h = i.create("hello0");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello0");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 0);
        i.destroy(h);
    }
    {
        auto const h = i.create("hello1");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello1");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 1);
        i.destroy(h);
    }
    {
        auto const h = i.create("hello2");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello2");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 2);
        i.destroy(h);
    }
}

static void create_reuses_last_deleted_slots() {
    auto i        = HandleVector<std::string> {};
    (void)i.create("hello0");
    (void)i.create("hello1");
    (void)i.create("hello2");
    auto const h3 = i.create("hello3");
    (void)i.create("hello4");
    i.destroy(h3);
    auto const h5 = i.create("hello5");
    auto const* v = i.get(h5);
    CHECK(v != nullptr && *v == "hello5");
    CHECK(h5.index() == h3.index());
    CHECK(h5.generation() == h3.generation() + 1);
}

int main() {
    RUN_SUITE(test_create_returns_valid_handle);
    RUN_SUITE(test_get_returns_value_for_valid_handle);
    RUN_SUITE(test_get_returns_none_for_dead_handle);
    RUN_SUITE(test_create_destroy_create_reuses_slot);
    RUN_SUITE(create_reuses_last_deleted_slots);
    return REPORT();
}
