// tests/test_small_vector.cpp -- the inline-storage vector
// (ecs/detail/small_vector.hpp) that backs WorkItem::units. Covers the inline
// case, the heap spill past N, and -- the reason it is a variant rather than a
// hand-rolled inline buffer -- that a move never dangles. The move tests scope
// the source so it is destroyed before the moved-to value is read, giving ASan
// teeth: an inline buffer wrongly aliased across a move would be a use-after-
// scope, a stolen heap buffer wrongly shared would be a use-after-free.

#include "check.hpp"
#include "ecs/detail/small_vector.hpp"
#include <utility>

using ecs::detail::SmallVector;

// A fresh SmallVector is empty and its range is degenerate.
static void empty_is_empty() {
    SmallVector<int, 2> v;
    CHECK(v.size() == 0);
    CHECK(v.empty());
    CHECK(v.begin() == v.end());
}

// Up to N elements live inline; values and order are preserved.
static void inline_holds_up_to_n() {
    SmallVector<int, 4> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    CHECK(v.size() == 3);
    CHECK(!v.empty());
    CHECK(v.begin()[0] == 10);
    CHECK(v.begin()[1] == 20);
    CHECK(v.begin()[2] == 30);
}

// Pushing past N spills to the heap while preserving every earlier element and
// their order; the container keeps growing correctly well beyond N.
static void spills_past_n_preserving_order() {
    SmallVector<int, 2> v;
    for (int i = 0; i < 10; ++i)
        v.push_back(i * 100);
    CHECK(v.size() == 10);
    bool ordered = true;
    for (int i = 0; i < 10; ++i)
        ordered = ordered && (v.begin()[i] == i * 100);
    CHECK(ordered);
}

// Range-for visits every element in order (what Query's for_each* rely on).
static void iterates_in_order() {
    SmallVector<int, 2> v;
    for (int i = 1; i <= 5; ++i)
        v.push_back(i);
    int sum = 0, count = 0;
    for (int const x : v) {
        sum += x;
        ++count;
    }
    CHECK(count == 5);
    CHECK(sum == 15);
}

// Moving an INLINE value must copy the buffer, not alias the source's -- read
// after the source is destroyed. (ASan catches an aliased inline buffer here.)
static void move_inline_does_not_dangle() {
    SmallVector<int, 2> dst;
    {
        SmallVector<int, 2> src;
        src.push_back(7);
        src.push_back(9);
        dst = std::move(src);
    }
    CHECK(dst.size() == 2);
    CHECK(dst.begin()[0] == 7);
    CHECK(dst.begin()[1] == 9);
}

// Moving a SPILLED value steals the heap buffer; the moved-to value owns it
// after the source is gone. (ASan catches a double-owned/freed buffer here.)
static void move_spilled_steals_buffer() {
    SmallVector<int, 1> dst;
    {
        SmallVector<int, 1> src;
        for (int i = 0; i < 6; ++i)
            src.push_back(i);
        dst = std::move(src);
    }
    CHECK(dst.size() == 6);
    int sum = 0;
    for (int const x : dst)
        sum += x;
    CHECK(sum == 15); // 0+1+2+3+4+5
}

// A copy is a deep, independent value (spilled case: distinct heap buffers).
static void copy_is_independent() {
    SmallVector<int, 1> a;
    for (int i = 0; i < 4; ++i)
        a.push_back(i);
    SmallVector<int, 1> b = a;
    b.push_back(99);
    CHECK(a.size() == 4);
    CHECK(b.size() == 5);
    CHECK(a.begin()[0] == 0);
    CHECK(b.begin()[4] == 99);
}

int main() {
    RUN_SUITE(empty_is_empty);
    RUN_SUITE(inline_holds_up_to_n);
    RUN_SUITE(spills_past_n_preserving_order);
    RUN_SUITE(iterates_in_order);
    RUN_SUITE(move_inline_does_not_dangle);
    RUN_SUITE(move_spilled_steals_buffer);
    RUN_SUITE(copy_is_independent);
    return REPORT();
}
