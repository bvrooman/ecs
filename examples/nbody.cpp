// examples/nbody.cpp
//
// End-to-end demonstration of the whole library in one frame loop:
//   * components are plain structs; an entity is whatever components it has
//   * those structs are stored field-wise (SoA) automatically via reflection
//   * a resource (Gravity) is a shared singleton
//   * systems run on a persistent worker pool, data-parallel within each system,
//     ordered automatically by their declared component/resource access
//   * an emitter spawns short-lived particles through the command buffer, using
//     the handle reservation returns; a reaper destroys them -- all deferred
//   * an extract system publishes a snapshot that two consumer threads (a mock
//     "renderer" and "audio" mixer) read concurrently via multi-consumer fan-out
//
// Build Release for meaningful timing: cmake -DCMAKE_BUILD_TYPE=Release

#include <ecs/ecs.hpp>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace ecs;

// --- components -----------------------------------------------------------
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
}; // ticks remaining before the reaper removes it
struct Tracer {}; // tag: the entities we visualize

// --- resources (singletons, not attached to any entity) -------------------
struct Gravity {
    float accel;
};

// The snapshot handed across the thread boundary: just the tracer positions.
// The library knows nothing about what this is "for".
using RenderSnapshot = std::vector<Position>;

int main() {
    constexpr int kParticles   = 200'000;
    constexpr int kTicks       = 120;
    constexpr float kDt        = 0.016f;
    constexpr int kEmitPerTick = 200;
    constexpr int kTracerLife  = 8;

    World world;
    world.emplace_resource<Gravity>(-9.81f);
    world.emplace_resource<SnapshotChannel<RenderSnapshot>>();

    // Initial population: a one-shot setup system, run inline.
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
        init.run(world); // inline, no thread pool needed
    }
    std::printf("spawned %zu entities\n", world.size());

    WorkerPool pool {std::max(2u, std::thread::hardware_concurrency())};

    Schedule schedule;

    // Access is derived from each system's parameter types: a `const` query
    // component or Res<T> is a read, a non-const component or ResMut<T> a write.

    // gravity: read Mass + the Gravity resource, write Velocity.
    schedule.add_kernel("gravity", [](Query<Velocity, Mass const> q, Res<Gravity> g) {
        float const a = g->accel;
        q.for_each_chunk([a](std::span<Entity const>, chunk<Velocity> vel, chunk<Mass const>) {
            for (auto& vy : vel.column<1>())
                vy += a * kDt; // SoA fast path
        });
    });

    // emitter: spawn short-lived tracer particles via Commands, using the handle
    // reservation hands back to attach follow-up components.
    schedule.add("emitter", [](Commands& cmd) {
        for (int i = 0; i < kEmitPerTick; ++i) {
            Entity e = cmd.spawn(Position {0, 0, 0},
                                 Velocity {float(i % 7) - 3, 5, 0},
                                 Mass {1});
            cmd.add<Lifetime>(e, Lifetime {kTracerLife});
            cmd.add<Tracer>(e, Tracer {});
        }
    });

    // reaper: age every Lifetime and destroy the expired ones (deferred).
    schedule.add("reaper", [](Query<Lifetime> q, Commands& cmd) {
        q.for_each([&](Entity e, auto& l) {
            if (--l.ticks <= 0)
                cmd.destroy(e);
        });
    });

    // integrate: read Velocity, write Position. Reads what gravity wrote, so the
    // scheduler places it on a later level automatically.
    schedule.add_kernel("integrate", [](Query<Position, Velocity const> q) {
        q.for_each_chunk([](std::span<Entity const>,
                            chunk<Position> pos,
                            chunk<Velocity const> vel) {
            auto px = pos.column<0>();
            auto py = pos.column<1>();
            auto pz = pos.column<2>();
            auto vx = vel.column<0>();
            auto vy = vel.column<1>();
            auto vz = vel.column<2>();
            for (std::size_t i = 0; i < px.size(); ++i) {
                px[i] += vx[i] * kDt;
                py[i] += vy[i] * kDt;
                pz[i] += vz[i] * kDt;
            }
        });
    });

    // extract: read tracer Positions (written by integrate -> later level) and
    // publish them to the snapshot channel for the consumer threads.
    schedule.add("extract",
                 [](Query<Position const, Tracer const> q,
                    ResMut<SnapshotChannel<RenderSnapshot>> ch) {
                     RenderSnapshot& out = ch->back();
                     out.clear();
                     q.for_each([&](auto& p, auto&) {
                         out.push_back({p.x, p.y, p.z});
                     });
                     ch->publish();
                 });

    std::printf("schedule: %zu systems across %zu parallel levels\n",
                schedule.size(),
                schedule.level_count());

    // Two independent consumers reading the SAME channel at their own pace.
    std::atomic<bool> stop {false};
    auto run_consumer =
        [&](char const* name, std::atomic<long>& frames, std::atomic<long>& items) {
            auto reader = world.resource<SnapshotChannel<RenderSnapshot>>().reader();
            long f = 0, last = 0;
            while (!stop.load(std::memory_order_acquire)) {
                if (reader.poll()) {
                    ++f;
                    last = long(reader.get()->size());
                } else {
                    std::this_thread::yield();
                }
            }
            if (reader.poll())
                last = long(reader.get()->size());
            frames.store(f);
            items.store(last);
            (void)name;
        };
    std::atomic<long> render_frames {0}, render_items {0};
    std::atomic<long> audio_frames {0}, audio_items {0};
    std::thread renderer(run_consumer,
                         "render",
                         std::ref(render_frames),
                         std::ref(render_items));
    std::thread audio(run_consumer,
                      "audio",
                      std::ref(audio_frames),
                      std::ref(audio_items));

    auto t0 = std::chrono::steady_clock::now();
    for (int tick = 0; tick < kTicks; ++tick)
        schedule.run(world, pool);
    auto t1 = std::chrono::steady_clock::now();

    stop.store(true, std::memory_order_release);
    renderer.join();
    audio.join();

    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("ran %d ticks in %.2f ms (%.3f ms/tick)\n", kTicks, ms, ms / kTicks);
    std::printf("entities after churn: %zu (%d field + live tracers)\n",
                world.size(),
                kParticles);
    std::printf("render consumer: %ld snapshots, last had %ld tracers\n",
                render_frames.load(),
                render_items.load());
    std::printf("audio  consumer: %ld snapshots, last had %ld tracers\n",
                audio_frames.load(),
                audio_items.load());

    Position p = world.get<Position>(Entity::from_raw(0, 0));
    std::printf("entity 0 final position = (%.3f, %.3f, %.3f)\n", p.x, p.y, p.z);
    return 0;
}
