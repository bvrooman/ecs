// tests/test_stats.cpp -- the timing-diagnostics utility (ecs/diag/stats.hpp).

#include <ecs/diag/stats.hpp>

#include "check.hpp"

using ecs::diag::TickStats;

// Reduce a known set of samples (0,1,...,99 us) to its distribution.
static void summary_over_known_samples() {
    TickStats ts;
    for (int i = 0; i < 100; ++i)
        ts.sample(static_cast<double>(i));
    CHECK(ts.pending() == 100);

    auto const s = ts.flush();
    CHECK(s.has_value());
    CHECK(s->n == 100);
    CHECK(s->mean == 49.5); // (0 + 1 + ... + 99) / 100
    CHECK(s->max == 99.0);
    CHECK(s->p50 == 50.0);                 // samples[100 * 0.50] = samples[50]
    CHECK(s->p99 == 99.0);                 // samples[min(99, 99)]
    CHECK((s->sd > 28.8 && s->sd < 28.9)); // population sd of 0..99 ~= 28.866

    CHECK(ts.pending() == 0);       // flush() consumes the samples
    CHECK(!ts.flush().has_value()); // nothing left
}

// No samples -> nullopt (and due() is not yet due right after construction).
static void empty_is_nullopt() {
    TickStats ts;
    CHECK(!ts.flush().has_value());
    CHECK(!ts.due().has_value());
}

// The RAII Scope records exactly one sample when it is destroyed.
static void scope_samples_once() {
    TickStats ts;
    {
        auto guard = ts.time();
    }
    CHECK(ts.pending() == 1);
}

int main() {
    RUN_SUITE(summary_over_known_samples);
    RUN_SUITE(empty_is_nullopt);
    RUN_SUITE(scope_samples_once);
    return REPORT();
}
