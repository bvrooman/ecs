// tests/wasm/smoke_mt.cpp
//
// The core on wasm threads. A real multi-lane WorkerPool -- Web Worker lanes
// under -pthread -- runs parallel systems that integrate, fold a Reduce, and
// record structural edits, and every result must match a 1-lane run exactly:
// the lane-count invariance the scheduler promises, now checked on wasm32's
// atomics, std::thread and shared memory. Headless; run with
//   node build-wasm/tests/wasm/ecs_smoke_mt.js
// It also builds and runs natively, which is a quick way to check a change to
// it before CI's Emscripten leg does.

#include <ecs/ecs.hpp>

#include <cstddef>
#include <cstdio>
#include <vector>

using namespace ecs;

struct Position {
    float x, y;
};
struct Velocity {
    float x, y;
};
struct Health {
    int hp;
};
struct Spawned {
    int v;
};

// Reduce target: a double sum, folded from per-item partials in item order.
struct Sum {
    double v = 0;
};
struct AddSum {
    void operator()(Sum& into, Sum& part) const { into.v += part.v; }
};

static int fails = 0;
static void check(bool const ok, char const* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fails += ok ? 0 : 1;
}

struct Outcome {
    std::vector<float> xs;    // Position.x in walk order: integrate + row layout
    std::vector<int> spawned; // Spawned.v in walk order: Commands apply order
    double sum        = 0;    // the last tick's Reduce
    std::size_t alive = 0;
    unsigned lanes    = 0;
};

static Outcome run(unsigned const lanes) {
    constexpr int kRows  = 40'000;
    constexpr int kTicks = 5;

    World w;
    w.emplace_resource<Sum>();
    {
        Schedule init;
        init.add_serial(
            "populate",
            [](Commands& cmd) {
                for (int i = 0; i < kRows; ++i)
                    cmd.spawn(Position {float(i), 0},
                              Velocity {0.5f, float(i % 13) * 0.25f},
                              Health {i});
            },
            times {1});
        init.run(w);
    }

    Schedule s;
    s.add_parallel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x;
            p.y += v.y;
        });
    });
    // Structural edits from every lane: destroys swap-and-pop rows, spawns
    // append them, so a lane-dependent apply order would change the layout.
    s.add_parallel("cull", [](Query<Health const> q, Commands& cmd) {
        q.for_each([&](Entity e, auto& h) {
            if (h.hp % 97 == 3)
                cmd.destroy(e);
            else if (h.hp % 89 == 0)
                cmd.spawn(Spawned {h.hp});
        });
    });
    s.add_parallel("measure", [](Query<Position const> q, Reduce<Sum, AddSum> sum) {
        q.for_each([&](auto& p) { sum->v += double(p.x) * 1e-3 + double(p.y); });
    });

    WorkerPool pool {lanes};
    for (int t = 0; t < kTicks; ++t)
        s.run(w, pool);

    Outcome out;
    w.for_each<Position const>([&](auto& p) { out.xs.push_back(p.x); });
    w.for_each<Spawned const>([&](auto& sp) { out.spawned.push_back(sp.v); });
    out.sum   = w.resource<Sum>().v;
    out.alive = w.size();
    out.lanes = pool.lanes();
    return out;
}

int main() {
    std::printf("=== ecs wasm smoke (threads) ===\n");
    auto const one  = run(1);
    auto const four = run(4);

    check(four.lanes == 4, "a 4-lane WorkerPool (3 worker threads)");
    check(!one.spawned.empty() && one.alive < 40'000 + one.spawned.size(),
          "the parallel systems spawned and destroyed rows");
    check(four.xs == one.xs, "positions and row layout match the 1-lane run");
    check(four.spawned == one.spawned, "spawns applied in the 1-lane order");
    check(four.sum == one.sum, "Reduce result bitwise equal to the 1-lane run");
    check(four.alive == one.alive, "same entity count");

    std::printf("=== %s ===\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
