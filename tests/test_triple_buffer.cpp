// Generic lock-free triple buffer (snapshot handoff) tests.
#include "check.hpp"
#include "setup.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace ecs;

static void single_threaded_semantics() {
    TripleBuffer<int> tb;
    CHECK(!tb.has_fresh());
    CHECK(!tb.consume()); // nothing published yet

    tb.back() = 7;
    tb.publish();
    CHECK(tb.has_fresh());
    CHECK(tb.consume());
    CHECK(tb.front() == 7);
    CHECK(!tb.consume()); // consumed; nothing new

    // Latest-value-wins: two publishes, one consume -> see the newest.
    tb.back() = 10;
    tb.publish();
    tb.back() = 20;
    tb.publish();
    CHECK(tb.consume());
    CHECK(tb.front() == 20);
    CHECK(!tb.consume());
}

// A snapshot whose fields must all agree; any disagreement the consumer sees
// would mean it read a buffer the producer was mid-write on (tearing).
struct Frame {
    int seq = 0;
    std::array<int, 64> data {};
};

static void concurrent_no_tearing() {
    TripleBuffer<Frame> tb;
    constexpr int kFrames = 200'000;
    std::atomic<bool> done {false};
    std::atomic<int> torn {0};
    int last_seq = -1;
    int observed = 0;

    std::thread producer([&] {
        for (int s = 1; s <= kFrames; ++s) {
            Frame& f = tb.back();
            f.seq    = s;
            for (int& v : f.data)
                v = s; // all fields carry the same seq
            tb.publish();
        }
        done.store(true, std::memory_order_release);
    });

    // Consumer: every snapshot it sees must be internally consistent and seq must
    // never go backwards.
    while (!done.load(std::memory_order_acquire) || tb.has_fresh()) {
        if (!tb.consume())
            continue;
        Frame const& f = tb.front();
        int const seq  = f.seq; // the snapshot is stable until next consume()
        for (int v : f.data)
            if (v != seq)
                ++torn; // any mismatch == read a half-written buffer
        if (seq < last_seq)
            ++torn; // published order must be preserved
        last_seq = seq;
        ++observed;
    }
    producer.join();

    CHECK(torn.load() == 0);
    CHECK(observed > 0);        // saw at least some snapshots
    CHECK(last_seq == kFrames); // and the final one
}

// Demonstrates the intended use: a schedule system extracts a snapshot from the
// SoA columns into a TripleBuffer resource and publishes; a consumer thread
// reads it. The library has no idea this snapshot is "for rendering" etc.
struct Position {
    float x, y;
};
using PositionSnapshot = std::vector<Position>;

static void world_extraction_handoff() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 1000; ++i)
            cmd.spawn(Position {static_cast<float>(i), 0});
    });
    w.emplace_resource<TripleBuffer<PositionSnapshot>>();

    std::atomic stop {false};
    std::atomic max_seen {0};

    // Consumer thread: read whatever has been published so far.
    std::thread consumer([&] {
        auto& tb = w.resource<TripleBuffer<PositionSnapshot>>();
        while (!stop.load(std::memory_order_acquire)) {
            if (tb.consume()) {
                PositionSnapshot const& snap = tb.front();
                max_seen.store(int(snap.size()), std::memory_order_relaxed);
            }
        }
        tb.consume();
        max_seen.store(int(tb.front().size()), std::memory_order_relaxed);
    });

    Schedule sched;
    sched.add_serial("extract",
                     [](Query<Position const> q,
                        ResMut<TripleBuffer<PositionSnapshot>> tb) {
                         PositionSnapshot& out = tb->back();
                         out.clear();
                         q.for_each([&](auto& p) { out.push_back({p.x, p.y}); });
                         tb->publish();
                     });

    WorkerPool pool {4};
    for (int frame = 0; frame < 50; ++frame)
        sched.run(w, pool);

    stop.store(true, std::memory_order_release);
    consumer.join();
    CHECK(max_seen.load() == 1000); // consumer observed the full snapshot
}

int main() {
    RUN_SUITE(single_threaded_semantics);
    RUN_SUITE(concurrent_no_tearing);
    RUN_SUITE(world_extraction_handoff);
    return REPORT();
}
