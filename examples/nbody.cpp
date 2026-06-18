// examples/nbody.cpp
//
// End-to-end demonstration of every feature:
//   * components are plain structs
//   * an entity is whatever group of components you give it
//   * those structs are stored field-wise (SoA) automatically via reflection
//   * systems run on a std::execution scheduler, in parallel where independent
//
// It simulates a simple particle field and prints timing for the SoA fast path.

#include "ecs/ecs.hpp"

#include <chrono>
#include <cstdio>
#include <exec/static_thread_pool.hpp>

using namespace ecs;

// --- components are just structs ------------------------------------------
struct Position { float x, y, z; };
struct Velocity { float x, y, z; };
struct Mass { float kg; };
struct Renderable {}; // tag component

// --- resources are singletons, not attached to any entity -----------------
struct Gravity { float accel; }; // shared simulation parameter

int main() {
  constexpr int kParticles = 200'000;
  constexpr int kTicks = 60;
  constexpr float kDt = 0.016f;

  World world;
  // An entity "defines a group of components" simply by which ones it has.
  for (int i = 0; i < kParticles; ++i) {
    float f = float(i);
    world.create_with(Position{f, -f, 0.5f * f},
                      Velocity{0.1f, -0.2f, 0.05f}, Mass{1.0f + 0.001f * f},
                      Renderable{});
  }
  std::printf("spawned %zu entities\n", world.size());

  // A resource the systems share by reference.
  world.emplace_resource<Gravity>(-9.81f);

  exec::static_thread_pool pool{std::max(1u, std::thread::hardware_concurrency())};
  auto scheduler = pool.get_scheduler();

  Schedule schedule;

  // Gravity: read the Gravity resource and Mass, write Velocity.
  schedule.add(
      "gravity",
      [](World& w) {
        const float a = w.resource<Gravity>().accel; // resource access
        query<Velocity, Mass>(w).for_each_chunk(
            [a](std::span<Entity>, soa_storage<Velocity>& vel,
                soa_storage<Mass>&) {
              // SoA fast path: pull the contiguous y-velocity column and
              // update it in a tight, vectorizer-friendly loop.
              for (float& vy : vel.column<1>()) vy += a * kDt;
            });
      },
      reads<Mass>{}, writes<Velocity>{}, reads_res<Gravity>{});

  // Integrate: read Velocity, write Position. Because it reads Velocity that
  // gravity writes, the scheduler places it on a later level automatically.
  schedule.add(
      "integrate",
      [](World& w) {
        query<Position, Velocity>(w).for_each_chunk(
            [](std::span<Entity>, soa_storage<Position>& pos,
               soa_storage<Velocity>& vel) {
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
      },
      reads<Velocity>{}, writes<Position>{});

  std::printf("schedule: %zu systems across %zu parallel levels\n",
              schedule.size(), schedule.level_count());

  auto t0 = std::chrono::steady_clock::now();
  for (int tick = 0; tick < kTicks; ++tick) schedule.run(world, scheduler);
  auto t1 = std::chrono::steady_clock::now();

  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("ran %d ticks in %.2f ms (%.3f ms/tick, %.1f M entity-updates/s)\n",
              kTicks, ms, ms / kTicks,
              double(kParticles) * kTicks / (ms / 1000.0) / 1e6);

  // Reassemble one entity (AoS view) to show round-tripping works.
  Position p = world.get<Position>(Entity{0, 0});
  std::printf("entity 0 final position = (%.3f, %.3f, %.3f)\n", p.x, p.y, p.z);
  return 0;
}
