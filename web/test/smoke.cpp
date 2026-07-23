// web/smoke.cpp
//
// Phase 0 WebAssembly smoke test: prove the ECS *core* compiles and runs under
// Emscripten with the portable (non-P2996) reflection backend, single-threaded.
//
// It deliberately drives the schedule only through Schedule::run(World&) -- a
// 1-lane WorkerPool that spawns no std::thread (see parallel/worker_pool.hpp) --
// so everything runs inline on the single wasm thread, with no -pthread and no
// SharedArrayBuffer. There is no graphics: results go to stdout, so running it
// with `node ecs_smoke.js` is enough to confirm the toolchain end-to-end.
//
// What it exercises (the three Phase 0 risks):
//   * the portable reflection backend (field_count_v on plain structs)
//   * recent libc++ features the core leans on (std::move_only_function in
//     Schedule; std::atomic<std::shared_ptr> in SnapshotChannel)
//   * the single-threaded core: deferred spawn/destroy through the Commands
//     command buffer, and automatic ordering across parallel levels
//     (gravity writes Velocity -> integrate reads it on a later level)

#include "ecs/ecs.hpp"
#include "ecs/reflection/reflect.hpp"
#include <cstdio>
#include <vector>

using namespace ecs;

// --- components: plain aggregates, stored field-wise (SoA) via reflection ----
struct Position {
    float x, y, z;
};
struct Velocity {
    float x, y, z;
};
struct Mass {
    float kg;
};
struct Lifetime {
    int ticks;
};
struct Tracer {}; // tag

// --- resource (singleton) ----------------------------------------------------
struct Gravity {
    float accel;
};

using Snapshot = std::vector<Position>;

int main() {
    constexpr int kParticles = 50'000;
    constexpr int kTicks     = 60;
    constexpr float kDt      = 0.016f;
    constexpr int kEmit      = 100;
    constexpr int kLife      = 5;

    std::printf("=== ecs wasm smoke ===\n");

    // Risk 1: the portable backend must see the fields of plain structs.
    std::printf("reflect: Position=%zu fields, Velocity=%zu, Mass=%zu, Lifetime=%zu\n",
                reflect::field_count_v<Position>,
                reflect::field_count_v<Velocity>,
                reflect::field_count_v<Mass>,
                reflect::field_count_v<Lifetime>);

    World world;
    world.emplace_resource<Gravity>(-9.81f);
    world.emplace_resource<SnapshotChannel<Snapshot>>();

    // Initial population: a one-shot setup system, deferred through Commands.
    {
        Schedule init;
        init.add_once("populate", [&](Commands& cmd) {
            for (int i = 0; i < kParticles; ++i) {
                float f = float(i);
                cmd.spawn(Position {f, -f, 0.5f * f},
                          Velocity {0.1f, -0.2f, 0.05f},
                          Mass {1.0f + 0.001f * f});
            }
        });
        init.run(world); // inline: builds WorkerPool{1}, spawns no thread
    }
    std::printf("spawned %zu entities\n", world.size());

    Schedule schedule;

    // gravity: read Mass + the Gravity resource, write Velocity.
    schedule.add("gravity", [](Query<Velocity, Mass const> q, Res<Gravity> g) {
        float const a = g->accel;
        q.for_each([a](auto& v, auto&) { v.y += a * kDt; });
    });

    // emitter: spawn short-lived tracer particles via the command buffer.
    schedule.add("emitter", [](Commands& cmd) {
        for (int i = 0; i < kEmit; ++i) {
            Entity e = cmd.spawn(Position {0, 0, 0},
                                 Velocity {float(i % 7) - 3, 5, 0},
                                 Mass {1});
            cmd.add<Lifetime>(e, Lifetime {kLife});
            cmd.add<Tracer>(e, Tracer {});
        }
    });

    // reaper: age every Lifetime, destroy the expired ones (deferred).
    schedule.add("reaper", [](Query<Lifetime> q, Commands& cmd) {
        q.for_each([&](Entity e, auto& l) {
            if (--l.ticks <= 0)
                cmd.destroy(e);
        });
    });

    // integrate: read Velocity (written by gravity -> later level), write Position.
    schedule.add("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x * kDt;
            p.y += v.y * kDt;
            p.z += v.z * kDt;
        });
    });

    // extract: publish tracer positions to the snapshot channel.
    schedule.add("extract",
                 [](Query<Position const, Tracer const> q,
                    ResMut<SnapshotChannel<Snapshot>> ch) {
                     Snapshot& out = ch->back();
                     out.clear();
                     q.for_each([&](auto& p, auto&) {
                         out.push_back({p.x, p.y, p.z});
                     });
                     ch->publish();
                 });

    std::printf("schedule: %zu systems across %zu parallel levels\n",
                schedule.size(),
                schedule.level_count());

    for (int t = 0; t < kTicks; ++t)
        schedule.run(world); // inline single-threaded tick

    // Risk 2: read the channel on this same thread (atomic<shared_ptr> at runtime).
    auto reader = world.resource<SnapshotChannel<Snapshot>>().reader();
    long snap   = reader.poll() ? long(reader.get()->size()) : -1;

    Position p0 = world.get<Position>(Entity::from_raw(0, 0));
    std::printf("after %d ticks: %zu entities alive, last snapshot = %ld tracers\n",
                kTicks,
                world.size(),
                snap);
    std::printf("entity 0 final position = (%.3f, %.3f, %.3f)\n", p0.x, p0.y, p0.z);
    std::printf("=== ok ===\n");
    return 0;
}
