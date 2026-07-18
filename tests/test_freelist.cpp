// tests/test_freelist.cpp -- the lock-free free list
// (ecs/detail/freelist.hpp). Exercises its single-threaded semantics; the
// lock-free allocate() path is covered under TSan by the schedule/churn tests.

#include "check.hpp"
#include "ecs/detail/freelist.hpp"

using ecs::detail::Freelist;

// With nothing freed, allocate() mints brand-new indices 0, 1, 2, ...
static void mints_new_indices() {
    Freelist fl;
    CHECK(fl.allocate() == 0);
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 2);
}

// Freed indices are recycled before new ones are minted, most-recently-freed
// first (LIFO); once drained, allocation resumes minting.
static void recycles_before_minting() {
    Freelist fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1
    (void)fl.allocate(); // 2
    fl.deallocate(1);
    fl.deallocate(2);
    CHECK(fl.allocate() == 2); // LIFO: most-recently-freed first
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 3); // chain drained -> mint
}

// Popping the chain unlinks in place, so a "wave" allocate() interleaves with a
// later deallocate() with no compaction step in between: the just-freed index
// simply becomes the new head.
static void pop_then_push_needs_no_compaction() {
    Freelist fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1
    (void)fl.allocate(); // 2
    fl.deallocate(1);
    fl.deallocate(2);          // chain head -> 2 -> 1
    CHECK(fl.allocate() == 2); // "wave" claims the head; chain head -> 1
    fl.deallocate(0);          // "barrier" frees a live index: head -> 0 -> 1
    CHECK(fl.allocate() == 0); // recycled, no reconciliation needed
    CHECK(fl.allocate() == 1); // remaining recycled slot
    CHECK(fl.allocate() == 3); // then mints anew
}

int main() {
    RUN_SUITE(mints_new_indices);
    RUN_SUITE(recycles_before_minting);
    RUN_SUITE(pop_then_push_needs_no_compaction);
    return REPORT();
}
