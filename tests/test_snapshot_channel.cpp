// Multi-consumer snapshot fan-out tests.
#include "check.hpp"
#include "setup.hpp"
#include <array>
#include <atomic>
#include <exec/static_thread_pool.hpp>
#include <thread>
#include <vector>

using namespace ecs;

static void single_threaded_semantics() {
    SnapshotChannel<int> ch;
    CHECK(ch.latest() == nullptr); // nothing published yet

    ch.back() = 5;
    ch.publish();
    auto s = ch.latest();
    CHECK(s != nullptr);
    CHECK(*s == 5);

    ch.back() = 6;
    ch.publish();
    CHECK(*ch.latest() == 6);
    CHECK(*s == 5); // previously held snapshot stays valid and unchanged
}

static void reader_detects_changes() {
    SnapshotChannel<int> ch;
    auto r = ch.reader();
    CHECK(!r.poll()); // nothing yet
    CHECK(r.get() == nullptr);

    ch.back() = 1;
    ch.publish();
    CHECK(r.poll()); // advanced
    CHECK(*r.get() == 1);
    CHECK(!r.poll()); // no new snapshot
    CHECK(*r.get() == 1);

    ch.back() = 2;
    ch.publish();
    CHECK(r.poll());
    CHECK(*r.get() == 2);
}

// A snapshot whose fields must all agree; any disagreement a consumer sees
// would mean it read a buffer the producer was mid-write on (impossible here,
// since published snapshots are immutable -- this asserts that property).
struct Frame {
    int seq = 0;
    std::array<int, 64> data {};
};

static void many_consumers_no_tearing() {
    SnapshotChannel<Frame> ch;
    constexpr int kFrames    = 100'000;
    constexpr int kConsumers = 4;

    std::atomic<bool> done {false};
    std::atomic<int> total_torn {0};
    std::array<std::atomic<int>, kConsumers> last_seq_seen {};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&, c] {
            auto r   = ch.reader();
            int last = 0, torn = 0;
            while (!done.load(std::memory_order_acquire) || r.poll()) {
                if (!r.poll())
                    continue;
                Frame const& f = *r.get();
                int const seq  = f.seq;
                for (int v : f.data)
                    if (v != seq)
                        ++torn; // immutable snapshot => must be consistent
                if (seq < last)
                    ++torn; // published order preserved
                last = seq;
            }
            total_torn.fetch_add(torn, std::memory_order_relaxed);
            last_seq_seen[c].store(last, std::memory_order_relaxed);
        });
    }

    // Single producer.
    for (int s = 1; s <= kFrames; ++s) {
        Frame& f = ch.back();
        f.seq    = s;
        for (int& v : f.data)
            v = s;
        ch.publish();
    }
    done.store(true, std::memory_order_release);
    for (auto& t : consumers)
        t.join();

    CHECK(total_torn.load() == 0);
    for (int c = 0; c < kConsumers; ++c) {
        CHECK(last_seq_seen[c].load() > 0);        // every consumer saw snapshots
        CHECK(last_seq_seen[c].load() <= kFrames); // and never past the end
    }
}

// Integration: a schedule system extracts world state into the channel; two
// independent consumer threads (e.g. a renderer and an audio mixer) read it.
struct Position {
    float x, y;
};
using Snapshot = std::vector<Position>;

static void world_extraction_fans_out_to_two_consumers() {
    World w;
    setup(w, [&](Commands& cmd) {
        for (int i = 0; i < 500; ++i)
            cmd.spawn(Position {float(i), 0});
    });
    w.emplace_resource<SnapshotChannel<Snapshot>>();

    std::atomic<bool> stop {false};
    std::array<std::atomic<int>, 2> seen {};

    auto consume = [&](int id) {
        auto r = w.resource<SnapshotChannel<Snapshot>>().reader();
        while (!stop.load(std::memory_order_acquire)) {
            if (r.poll())
                seen[id].store(int(r.get()->size()), std::memory_order_relaxed);
        }
        if (r.poll() && r.get())
            seen[id].store(int(r.get()->size()), std::memory_order_relaxed);
    };
    std::thread renderer(consume, 0);
    std::thread audio(consume, 1);

    Schedule sched;
    sched.add("extract",
              [](Query<Position const> q, ResMut<SnapshotChannel<Snapshot>> ch) {
                  Snapshot& out = ch->back();
                  out.clear();
                  q.for_each_chunk(
                      [&](std::span<Entity>, soa_storage<Position> const& pos) {
                          auto xs = pos.column<0>();
                          auto ys = pos.column<1>();
                          for (std::size_t i = 0; i < xs.size(); ++i)
                              out.push_back(Position {xs[i], ys[i]});
                      });
                  ch->publish();
              });

    exec::static_thread_pool pool {4};
    for (int frame = 0; frame < 50; ++frame)
        sched.run(w, pool.get_scheduler());

    stop.store(true, std::memory_order_release);
    renderer.join();
    audio.join();
    CHECK(seen[0].load() == 500); // both consumers observed the full snapshot
    CHECK(seen[1].load() == 500);
}

int main() {
    RUN_SUITE(single_threaded_semantics);
    RUN_SUITE(reader_detects_changes);
    RUN_SUITE(many_consumers_no_tearing);
    RUN_SUITE(world_extraction_fans_out_to_two_consumers);
    return REPORT();
}
