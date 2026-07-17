// tests/test_free_list.cpp -- the recycling index allocator
// (ecs/detail/free_list.hpp). Exercises its single-threaded semantics; the
// lock-free allocate() path is covered under TSan by the schedule/churn tests.

#include "check.hpp"
#include "ecs/detail/free_list.hpp"
#include "ecs/entity.hpp"
#include <cstdint>

using ecs::Entity;
using ecs::detail::FreeList;

// With nothing freed, allocate() mints brand-new handles 0, 1, 2, ...
static void mints_new_handles() {
    FreeList<std::uint32_t> fl;
    CHECK(fl.allocate() == 0);
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 2);
}

// Freed handles are recycled before new ones are minted, most-recently-freed
// first (LIFO); once drained, allocation resumes minting.
static void recycles_before_minting() {
    FreeList<std::uint32_t> fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1
    (void)fl.allocate(); // 2
    fl.free(1);
    fl.free(2);
    CHECK(fl.allocate() == 2); // LIFO
    CHECK(fl.allocate() == 1);
    CHECK(fl.allocate() == 3); // free list drained -> mint
}

// compact() drops the slots allocate() claimed (their handles are now live),
// restoring the cursor == free-size rest state so later frees stack cleanly.
static void compact_drops_claimed_tail() {
    FreeList<std::uint32_t> fl;
    (void)fl.allocate(); // 0
    (void)fl.allocate(); // 1
    fl.free(0);
    fl.free(1);              // free = [0, 1]
    CHECK(fl.allocate() == 1); // claims the tail; free vector still holds [0,1]
    fl.compact();              // drop the claimed slot -> free = [0]
    CHECK(fl.allocate() == 0); // recycles the remaining freed slot
    CHECK(fl.allocate() == 2); // then mints anew
}

// A composite handle round-trips: free() stores it whole and allocate()
// recycles it verbatim (so an Entity's generation survives), while a minted
// handle is built from the index counter as {index, 0}.
static void round_trips_composite_handles() {
    FreeList<Entity, std::uint32_t> fl;
    CHECK(fl.allocate() == (Entity {0, 0})); // minted from the index counter
    CHECK(fl.allocate() == (Entity {1, 0}));
    fl.free(Entity {0, 7});                   // freed with a bumped generation
    CHECK(fl.allocate() == (Entity {0, 7}));  // recycled verbatim
    CHECK(fl.allocate() == (Entity {2, 0}));  // drained -> mint next index
}

int main() {
    RUN_SUITE(mints_new_handles);
    RUN_SUITE(recycles_before_minting);
    RUN_SUITE(compact_drops_claimed_tail);
    RUN_SUITE(round_trips_composite_handles);
    return REPORT();
}
