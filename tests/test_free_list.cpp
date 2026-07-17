// tests/test_free_list.cpp -- the recycling index allocator
// (ecs/detail/free_list.hpp). Exercises its single-threaded semantics; the
// lock-free allocate() path is covered under TSan by the schedule/churn tests.

#include "check.hpp"
#include "ecs/detail/free_list.hpp"
#include <cstdint>

using ecs::detail::FreeList;

// With nothing freed, allocate() mints brand-new handles 0, 1, 2, ... and
// high_water() tracks the count handed out.
static void mints_new_handles() {
    FreeList<std::uint32_t> fl;
    CHECK(fl.high_water() == 0);
    CHECK(fl.allocate() == 0);
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 2);
    CHECK(fl.high_water() == 3);
}

// Freed handles are recycled before new ones are minted, most-recently-freed
// first (LIFO); once drained, allocation resumes minting.
static void recycles_before_minting() {
    FreeList<std::uint32_t> fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1
    (void)fl.allocate(); // 2  (high_water == 3)
    fl.free(1);
    fl.free(2);
    CHECK(fl.allocate() == 2); // LIFO
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 3); // free list drained -> mint
    CHECK(fl.high_water() == 4);
}

// compact() drops the slots allocate() claimed (their handles are now live),
// restoring the cursor == free-size rest state so later frees stack cleanly.
static void compact_drops_claimed_tail() {
    FreeList<std::uint32_t> fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1  (high_water == 2)
    fl.free(0);
    fl.free(1);              // free = [0, 1]
    CHECK(fl.allocate() == 1); // claims the tail; free vector still holds [0,1]
    fl.compact();              // drop the claimed slot -> free = [0]
    CHECK(fl.allocate() == 0); // recycles the remaining freed slot
    CHECK(fl.allocate() == 2); // then mints anew
}

int main() {
    RUN_SUITE(mints_new_handles);
    RUN_SUITE(recycles_before_minting);
    RUN_SUITE(compact_drops_claimed_tail);
    return REPORT();
}
