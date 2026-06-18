// Generic lock-free triple buffer (snapshot handoff) tests.
#include "check.hpp"
#include "ecs/ecs.hpp"

#include <array>
#include <atomic>
#include <exec/static_thread_pool.hpp>
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
  std::array<int, 64> data{};
};

static void concurrent_no_tearing() {
  TripleBuffer<Frame> tb;
  constexpr int kFrames = 200'000;
  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  int last_seq = -1;
  int observed = 0;

  std::thread producer([&] {
    for (int s = 1; s <= kFrames; ++s) {
      Frame& f = tb.back();
      f.seq = s;
      for (int& v : f.data) v = s; // all fields carry the same seq
      tb.publish();
    }
    done.store(true, std::memory_order_release);
  });

  // Consumer: every snapshot it sees must be internally consistent and seq must
  // never go backwards.
  while (!done.load(std::memory_order_acquire) || tb.has_fresh()) {
    if (!tb.consume()) continue;
    const Frame& f = tb.front();
    const int seq = f.seq;       // the snapshot is stable until next consume()
    for (int v : f.data)
      if (v != seq) ++torn;      // any mismatch == read a half-written buffer
    if (seq < last_seq) ++torn;  // published order must be preserved
    last_seq = seq;
    ++observed;
  }
  producer.join();

  CHECK(torn.load() == 0);
  CHECK(observed > 0);          // saw at least some snapshots
  CHECK(last_seq == kFrames);   // and the final one
}

// Demonstrates the intended use: a schedule system extracts a snapshot from the
// SoA columns into a TripleBuffer resource and publishes; a consumer thread
// reads it. The library has no idea this snapshot is "for rendering" etc.
struct Position { float x, y; };
using PositionSnapshot = std::vector<Position>;

static void world_extraction_handoff() {
  World w;
  for (int i = 0; i < 1000; ++i) w.create_with(Position{float(i), 0});
  w.emplace_resource<TripleBuffer<PositionSnapshot>>();

  std::atomic<bool> stop{false};
  std::atomic<int> max_seen{0};

  // Consumer thread: read whatever has been published so far.
  std::thread consumer([&] {
    auto& tb = w.resource<TripleBuffer<PositionSnapshot>>();
    while (!stop.load(std::memory_order_acquire)) {
      if (tb.consume()) {
        const PositionSnapshot& snap = tb.front();
        max_seen.store(int(snap.size()), std::memory_order_relaxed);
      }
    }
    tb.consume();
    max_seen.store(int(tb.front().size()), std::memory_order_relaxed);
  });

  Schedule sched;
  sched.add(
      "extract",
      [](World& wr) {
        auto& tb = wr.resource<TripleBuffer<PositionSnapshot>>();
        PositionSnapshot& out = tb.back();
        out.clear();
        query<Position>(wr).for_each_chunk(
            [&](std::span<Entity>, soa_storage<Position>& pos) {
              auto xs = pos.column<0>();
              auto ys = pos.column<1>();
              for (std::size_t i = 0; i < xs.size(); ++i)
                out.push_back(Position{xs[i], ys[i]});
            });
        tb.publish();
      },
      reads<Position>{}, reads_res<TripleBuffer<PositionSnapshot>>{});

  exec::static_thread_pool pool{4};
  for (int frame = 0; frame < 50; ++frame) sched.run(w, pool.get_scheduler());

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
