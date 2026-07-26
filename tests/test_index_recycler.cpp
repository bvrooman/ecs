// tests/test_index_recycler.cpp -- the recycling index allocator
// (ecs/detail/container/index_recycler.hpp). Exercises its single-threaded semantics; the
// lock-free allocate() path is covered under TSan by the schedule/churn tests.

#include <ecs/detail/container/index_recycler.hpp>

#include "check.hpp"

using ecs::detail::IndexRecycler;

// With nothing freed, allocate() mints brand-new indices 0, 1, 2, ...
static void mints_new_indices() {
    IndexRecycler r;
    CHECK(r.allocate() == 0);
    CHECK(r.allocate() == 1);
    CHECK(r.allocate() == 2);
}

// Freed indices are recycled before new ones are minted, most-recently-freed
// first (LIFO); once drained, allocation resumes minting.
static void recycles_before_minting() {
    IndexRecycler r;
    (void)r.allocate(); // 0
    (void)r.allocate(); // 1
    (void)r.allocate(); // 2
    r.free(1);
    r.free(2);
    CHECK(r.allocate() == 2); // LIFO
    CHECK(r.allocate() == 1);
    CHECK(r.allocate() == 3); // free stack drained -> mint
}

// compact() drops the slots allocate() claimed (their indices are now live),
// restoring the cursor == free-size rest state so later frees stack cleanly.
static void compact_drops_claimed_tail() {
    IndexRecycler r;
    (void)r.allocate(); // 0
    (void)r.allocate(); // 1
    r.free(0);
    r.free(1);                // free = [0, 1]
    CHECK(r.allocate() == 1); // claims the tail; free stack still holds [0,1]
    r.compact();              // drop the claimed slot -> free = [0]
    CHECK(r.allocate() == 0); // recycles the remaining freed slot
    CHECK(r.allocate() == 2); // then mints anew
}

int main() {
    RUN_SUITE(mints_new_indices);
    RUN_SUITE(recycles_before_minting);
    RUN_SUITE(compact_drops_claimed_tail);
    return REPORT();
}
