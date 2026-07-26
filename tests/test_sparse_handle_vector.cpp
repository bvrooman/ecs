#include <ecs/detail/container/sparse_handle_vector.hpp>

#include "check.hpp"
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using ecs::detail::Handle;
using ecs::detail::SparseHandleVector;

namespace {
struct StrTag {};
struct IntTag {};
using Strings   = SparseHandleVector<std::string, StrTag>;
using Ints      = SparseHandleVector<int, IntTag>;
using IntHandle = Handle<IntTag>;
} // namespace

static void create_returns_valid_handle() {
    auto i = Strings {};
    auto h = i.create("hello");
    CHECK(h.index() == 0);
    CHECK(h.generation() == 0);
    CHECK(i.size() == 1);
}

static void get_returns_value_for_valid_handle() {
    auto i    = Strings {};
    auto h    = i.create("hello");
    auto* got = i.get(h);
    CHECK(got != nullptr && *got == "hello");
}

static void get_returns_none_for_dead_handle() {
    auto i  = Strings {};
    auto h0 = i.create("hello");
    i.destroy(h0);
    (void)i.create("hello");
    CHECK(i.get(h0) == nullptr);
    CHECK(!i.is_alive(h0));
}

static void create_destroy_create_reuses_slot() {
    auto i = Strings {};
    for (std::uint32_t gen = 0; gen < 3; ++gen) {
        auto const h = i.create("hi");
        CHECK(h.index() == 0);
        CHECK(h.generation() == gen); // same slot, bumped generation each round
        CHECK(*i.get(h) == "hi");
        i.destroy(h);
    }
}

// The defining sparse property: destroy() leaves every other slot in place --
// no swap-and-pop -- so surviving handles are undisturbed and index-stable.
static void destroy_does_not_disturb_other_slots() {
    auto i        = Ints {};
    auto const h0 = i.create(10);
    auto const h1 = i.create(11);
    auto const h2 = i.create(12);
    CHECK(h0.index() == 0 && h1.index() == 1 && h2.index() == 2);
    i.destroy(h1); // punch a hole in the middle
    CHECK(i.size() == 2);
    CHECK(!i.is_alive(h1));
    // h0 and h2 keep their slots, values, and indices untouched.
    CHECK(i[h0] == 10 && h0.index() == 0);
    CHECK(i[h2] == 12 && h2.index() == 2);
    // The freed middle slot is the next one handed out (LIFO recycle).
    auto const h3 = i.create(13);
    CHECK(h3.index() == 1 && h3.generation() == 1);
    CHECK(i[h3] == 13);
}

static void create_accepts_lvalue_and_args() {
    auto i        = Strings {};
    std::string s = "lvalue";
    auto const h0 = i.create(s); // copy
    CHECK(s == "lvalue");
    auto const h1 = i.create(std::move(s)); // move
    auto const h2 = i.create(3, 'x');       // in-place std::string(3, 'x')
    CHECK(*i.get(h0) == "lvalue");
    CHECK(*i.get(h1) == "lvalue");
    CHECK(*i.get(h2) == "xxx");
}

// operator[] is mutable in place.
static void index_operator_gives_mutable_reference() {
    auto i       = Strings {};
    auto const h = i.create("hello");
    i[h] += " world";
    CHECK(i[h] == "hello world");
    auto const& ci = i;
    CHECK(ci[h] == "hello world");
}

// A reserved handle is not alive until commit() materializes its payload.
static void reserve_is_not_alive_until_commit() {
    auto i       = Strings {};
    auto const h = i.reserve();
    CHECK(!i.is_alive(h));
    CHECK(i.get(h) == nullptr);
    CHECK(i.size() == 0);
    i.commit(h, "materialized");
    CHECK(i.is_alive(h));
    CHECK(*i.get(h) == "materialized");
    CHECK(i.size() == 1);
    CHECK(i.destroy(h));
    CHECK(!i.is_alive(h));
    CHECK(i.size() == 0);
}

// reserve() is lock-free and safe under concurrency: every index handed out
// across threads is unique. Run under TSan this exercises the concurrent
// allocate() (CAS pop + fetch_add mint) path.
static void concurrent_reserve_yields_unique_indices() {
    auto i = Ints {};
    std::vector<IntHandle> seed;
    for (int n = 0; n < 500; ++n) {
        seed.push_back(i.create(n));
    }
    for (auto const h : seed) {
        (void)i.destroy(h); // 500 recycled slots
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
            CHECK(seen.insert(h.index()).second);
        }
    }
    CHECK(seen.size() == static_cast<std::size_t>(threads) * per);
}

int main() {
    RUN_SUITE(create_returns_valid_handle);
    RUN_SUITE(get_returns_value_for_valid_handle);
    RUN_SUITE(get_returns_none_for_dead_handle);
    RUN_SUITE(create_destroy_create_reuses_slot);
    RUN_SUITE(destroy_does_not_disturb_other_slots);
    RUN_SUITE(create_accepts_lvalue_and_args);
    RUN_SUITE(index_operator_gives_mutable_reference);
    RUN_SUITE(reserve_is_not_alive_until_commit);
    RUN_SUITE(concurrent_reserve_yields_unique_indices);
    return REPORT();
}
