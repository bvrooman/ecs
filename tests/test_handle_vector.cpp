#include "check.hpp"
#include <ecs/detail/container/handle_vector.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using ecs::detail::Handle;
using ecs::detail::HandleVector;

namespace {
struct StrTag {};
struct IntTag {};
using Strings   = HandleVector<std::string, StrTag>;
using Ints      = HandleVector<int, IntTag>;
using IntHandle = Handle<IntTag>;
} // namespace

static void test_create_returns_valid_handle() {
    auto i = Strings {};
    auto h = i.create("hello");
    CHECK(h.index() == 0);
    CHECK(h.generation() == 0);
}

static void test_get_returns_value_for_valid_handle() {
    auto i    = Strings {};
    auto h    = i.create("hello");
    auto* get = i.get(h);
    CHECK(get != nullptr && *get == "hello");
}

static void test_get_returns_none_for_dead_handle() {
    auto i  = Strings {};
    auto h0 = i.create("hello");
    i.destroy(h0);
    (void)i.create("hello");
    auto* get = i.get(h0);
    CHECK(get == nullptr);
}

static void test_create_destroy_create_reuses_slot() {
    auto i = Strings {};
    {
        auto const h  = i.create("hello0");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello0");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 0);
        i.destroy(h);
    }
    {
        auto const h  = i.create("hello1");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello1");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 1);
        i.destroy(h);
    }
    {
        auto const h  = i.create("hello2");
        auto const* v = i.get(h);
        CHECK(v != nullptr && *v == "hello2");
        CHECK(h.index() == 0);
        CHECK(h.generation() == 2);
        i.destroy(h);
    }
}

static void create_reuses_last_deleted_slots() {
    auto i        = Strings {};
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

// create() accepts an lvalue (copy), an rvalue (move), or T's constructor args.
static void create_accepts_lvalue_and_args() {
    auto i        = Strings {};
    std::string s = "lvalue";
    auto const h0 = i.create(s);            // copy from lvalue
    CHECK(s == "lvalue");                    // source left intact by the copy
    auto const h1 = i.create(std::move(s)); // move from rvalue
    auto const h2 = i.create(3, 'x');       // in-place: std::string(3, 'x')
    CHECK(*i.get(h0) == "lvalue");
    CHECK(*i.get(h1) == "lvalue");
    CHECK(*i.get(h2) == "xxx");
}

// operator[] is unchecked reference access for a known-live handle, and is
// mutable in place; get() is the checked pointer form.
static void index_operator_gives_mutable_reference() {
    auto i       = Strings {};
    auto const h = i.create("hello");
    CHECK(i[h] == "hello");
    i[h] += " world"; // mutate in place through the reference
    CHECK(i[h] == "hello world");
    CHECK(*i.get(h) == "hello world");
    // const overload
    auto const& ci = i;
    CHECK(ci[h] == "hello world");
}

// A reserved handle is not alive until commit() materializes its record; after
// destroy() it is stale again.
static void reserve_is_not_alive_until_commit() {
    auto i       = Strings {};
    auto const h = i.reserve();
    CHECK(!i.is_alive(h)); // reserved, not yet materialized
    CHECK(i.get(h) == nullptr);
    i.commit(h, "materialized");
    CHECK(i.is_alive(h));
    auto* v = i.get(h);
    CHECK(v != nullptr && *v == "materialized");
    CHECK(i.size() == 1);
    CHECK(i.destroy(h));
    CHECK(!i.is_alive(h)); // stale after destroy
    CHECK(i.get(h) == nullptr);
    CHECK(i.size() == 0);
}

// A reserved-but-uncommitted handle keeps its slot out of the alive set, and a
// later commit picks up where the recycler left off.
static void reserve_without_commit_leaves_no_live_record() {
    auto i        = Ints {};
    auto const h0 = i.reserve(); // index 0, never committed
    auto const h1 = i.create(42);
    CHECK(!i.is_alive(h0));
    CHECK(i.is_alive(h1));
    CHECK(i.size() == 1);
    CHECK(h0.index() != h1.index()); // distinct slots handed out
}

// from_raw rebuilds a handle from an external index+generation pair; it reads as
// the same live handle when the pair matches, and dead when it does not.
static void from_raw_round_trips_through_get() {
    auto i         = Strings {};
    auto const h   = i.create("hello");
    auto const raw = Handle<StrTag>::from_raw(h.index(), h.generation());
    CHECK(raw == h);
    CHECK(i.get(raw) != nullptr && *i.get(raw) == "hello");
    auto const stale = Handle<StrTag>::from_raw(h.index(), h.generation() + 1);
    CHECK(i.get(stale) == nullptr); // wrong generation -> dead
    auto const oob = Handle<StrTag>::from_raw(9999, 0);
    CHECK(i.get(oob) == nullptr); // out of range -> dead
}

// Handle is usable as an unordered_map key via the provided std::hash.
static void handle_is_hashable() {
    auto i       = Ints {};
    auto const a = i.create(1);
    auto const b = i.create(2);
    std::unordered_map<IntHandle, int> m;
    m[a] = 10;
    m[b] = 20;
    CHECK(m.at(a) == 10);
    CHECK(m.at(b) == 20);
    CHECK(m.size() == 2);
    CHECK(std::hash<IntHandle> {}(a) == std::hash<IntHandle> {}(a));
}

// reserve() is lock-free and safe under concurrency: every index handed out
// across threads is unique, whether recycled or freshly minted. Run under TSan
// this exercises the concurrent allocate() (CAS pop + fetch_add mint) path.
static void concurrent_reserve_yields_unique_indices() {
    auto i = Ints {};
    // Seed a free list so reserve() mixes recycled slots with minted ones.
    std::vector<IntHandle> seed;
    for (int n = 0; n < 500; ++n) {
        seed.push_back(i.create(n));
    }
    for (auto const h : seed) {
        (void)i.destroy(h); // 500 recycled slots on the free list
    }

    constexpr int threads = 8;
    constexpr int per     = 500;
    std::vector<std::vector<IntHandle>> claimed(threads);
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            for (int n = 0; n < per; ++n) {
                claimed[t].push_back(i.reserve());
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }

    std::unordered_set<std::uint32_t> seen;
    for (auto const& batch : claimed) {
        for (auto const h : batch) {
            CHECK(seen.insert(h.index()).second); // no index handed out twice
        }
    }
    CHECK(seen.size() == static_cast<std::size_t>(threads) * per);
}

int main() {
    RUN_SUITE(test_create_returns_valid_handle);
    RUN_SUITE(test_get_returns_value_for_valid_handle);
    RUN_SUITE(test_get_returns_none_for_dead_handle);
    RUN_SUITE(test_create_destroy_create_reuses_slot);
    RUN_SUITE(create_reuses_last_deleted_slots);
    RUN_SUITE(create_accepts_lvalue_and_args);
    RUN_SUITE(index_operator_gives_mutable_reference);
    RUN_SUITE(reserve_is_not_alive_until_commit);
    RUN_SUITE(reserve_without_commit_leaves_no_live_record);
    RUN_SUITE(from_raw_round_trips_through_get);
    RUN_SUITE(handle_is_hashable);
    RUN_SUITE(concurrent_reserve_yields_unique_indices);
    return REPORT();
}
