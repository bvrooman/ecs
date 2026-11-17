// tests/wasm/smoke.cpp
//
// WebAssembly smoke test: prove the ECS *core* compiles and runs under
// Emscripten with the portable (non-P2996) reflection backend, single-threaded.
// Run with `node build-wasm/tests/wasm/ecs_smoke.js`; it exits non-zero if a
// check fails. (smoke_mt.cpp covers the core on wasm threads.)
//
// It deliberately drives the schedule only through Schedule::run(World&) -- a
// 1-lane WorkerPool that spawns no std::thread (see parallel/worker_pool.hpp) --
// so everything runs inline on the single wasm thread, with no -pthread and no
// SharedArrayBuffer. There is no graphics: results go to stdout, so running it
// with `node ecs_smoke.js` is enough to confirm the toolchain end-to-end.
//
// What it exercises:
//   * the portable reflection backend (field_count_v on plain structs)
//   * recent libc++ features the core leans on (std::move_only_function in
//     Schedule; std::atomic<std::shared_ptr> in SnapshotChannel)
//   * the single-threaded core: deferred spawn/destroy through the Commands
//     command buffer, and automatic ordering across parallel levels
//     (gravity writes Velocity -> integrate reads it on a later level)
//   * the runtime-typed component module (ecs/dynamic.hpp: Registry,
//     DynamicColumn, WorldOps) that runtime-defined content and an editor
//     build on -- it is not part of ecs/ecs.hpp, so it is included here to
//     keep it compiling and working on wasm32 too

#include <ecs/dynamic.hpp>
#include <ecs/ecs.hpp>
#include <ecs/reflection/reflect.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

static int fails = 0;
static void check(bool const ok, char const* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++fails;
    }
}

// The runtime-typed path: a component described by a field list rather than a
// C++ struct, added and read back by ComponentId, then spawned through the
// deferred command buffer and applied at a flush.
static void dynamic_round_trip() {
    using namespace ecs::dynamic;
    struct Packed { // matches the descriptor below: two packed f32 fields
        float t, u;
    };
    auto const Heat =
        registry().define("Heat", {{"t", FieldType::f32}, {"u", FieldType::f32}});

    World w;
    Entity const e = WorldOps::create_entity(w);
    Packed const in {36.5f, -2.25f};
    WorldOps::add(w, e, Heat, &in);
    Packed out {};
    check(WorldOps::get(w, e, Heat, &out) && out.t == in.t && out.u == in.u,
          "dynamic component round-trips by ComponentId");

    std::vector<std::byte> blob(sizeof(Packed));
    std::memcpy(blob.data(), &in, sizeof in);
    Entity const spawned = WorldOps::spawn_deferred(w, {{Heat, std::move(blob)}});
    Schedule flush;
    flush.add_serial("flush", [] {});
    flush.run(w);
    check(w.size() == 2 && WorldOps::get(w, spawned, Heat, &out) && out.t == in.t,
          "deferred dynamic spawn applies at the flush");
    std::printf("dynamic: Heat id=%u, %zu entities\n", unsigned(Heat), w.size());
}

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
    check(reflect::field_count_v<Position> == 3 &&
              reflect::field_count_v<Velocity> == 3 &&
              reflect::field_count_v<Mass> == 1 && reflect::field_count_v<Lifetime> == 1,
          "portable reflection counts every field");

    World world;
    world.emplace_resource<Gravity>(-9.81f);
    world.emplace_resource<SnapshotChannel<Snapshot>>();

    // Initial population: a one-shot setup system, deferred through Commands.
    {
        Schedule init;
        init.add_serial(
            "populate",
            [&](Commands& cmd) {
                for (int i = 0; i < kParticles; ++i) {
                    float f = float(i);
                    cmd.spawn(Position {f, -f, 0.5f * f},
                              Velocity {0.1f, -0.2f, 0.05f},
                              Mass {1.0f + 0.001f * f});
                }
            },
            times {1});
        init.run(world); // inline: builds WorkerPool{1}, spawns no thread
    }
    std::printf("spawned %zu entities\n", world.size());
    check(world.size() == std::size_t(kParticles),
          "the setup system spawned every particle");

    Schedule schedule;

    // gravity: read Mass + the Gravity resource, write Velocity.
    schedule.add_serial("gravity", [](Query<Velocity, Mass const> q, Res<Gravity> g) {
        float const a = g->accel;
        q.for_each([a](auto& v, auto&) { v.y += a * kDt; });
    });

    // emitter: spawn short-lived tracer particles via the command buffer.
    schedule.add_serial("emitter", [](Commands& cmd) {
        for (int i = 0; i < kEmit; ++i) {
            Entity e = cmd.spawn(Position {0, 0, 0},
                                 Velocity {float(i % 7) - 3, 5, 0},
                                 Mass {1});
            cmd.add<Lifetime>(e, Lifetime {kLife});
            cmd.add<Tracer>(e, Tracer {});
        }
    });

    // reaper: age every Lifetime, destroy the expired ones (deferred).
    schedule.add_serial("reaper", [](Query<Lifetime> q, Commands& cmd) {
        q.for_each([&](Entity e, auto& l) {
            if (--l.ticks <= 0)
                cmd.destroy(e);
        });
    });

    // integrate: read Velocity (written by gravity -> later level), write Position.
    schedule.add_serial("integrate", [](Query<Position, Velocity const> q) {
        q.for_each([](auto& p, auto& v) {
            p.x += v.x * kDt;
            p.y += v.y * kDt;
            p.z += v.z * kDt;
        });
    });

    // extract: publish tracer positions to the snapshot channel.
    schedule.add_serial("extract",
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
    check(world.size() > std::size_t(kParticles),
          "tracers are alive alongside the particles");
    check(snap > 0, "the snapshot channel delivered the tracer positions");
    check(p0.x > 0.0f && p0.y < 0.0f, "gravity and integrate moved entity 0");

    dynamic_round_trip();

    std::printf("=== %s ===\n", fails == 0 ? "ok" : "FAILED");
    return fails == 0 ? 0 : 1;
}
