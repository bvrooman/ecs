# ecs

A header-only **Entity-Component-System** for modern C++ where you write
components as plain structs (array-of-structures) and they are stored
**structure-of-arrays automatically, using C++ reflection** — laid out for
cache-friendly, vectorizable iteration and run **data-parallel on a persistent
worker pool**, split across cores *within* each system, with safe ordering
derived from the components each system touches.

```cpp
#include "ecs/ecs.hpp"
using namespace ecs;

struct Position { float x, y, z; };   // components are just structs
struct Velocity { float x, y, z; };

int main() {
  World world;
  Schedule init;                                  // setup is a one-shot system
  init.add_serial("populate", [](Commands& cmd) {
    for (int i = 0; i < 100'000; ++i)             // an entity is whatever
      cmd.spawn(Position{}, Velocity{1, 0, 0});   // components you give it
  }, {}, /*every=*/1, /*times=*/1);               // times=1: runs once, then drops out
  init.run(world);                                // serial (a 1-lane pool)

  Schedule schedule;
  // A parallel system: its access is derived from its parameter types (here, write
  // Position, read const Velocity). `add_parallel` lets the executor slice the
  // matched rows into work items across the pool's lanes -- that IS the
  // parallelism. Each work item binds `q` to its own slice, so the body below
  // runs once per lane over disjoint rows (race-free by construction).
  schedule.add_parallel("integrate", [](Query<Position, const Velocity> q) {
    // for_each_chunk is the SoA access shape (not the source of parallelism): a
    // `chunk` per component whose column<I>() is that field's contiguous span
    // over this work item's slice -- no per-row gather/scatter.
    q.for_each_chunk([](std::span<Entity>,
                        chunk<Position> pos, chunk<const Velocity> vel) {
      auto px = pos.column<0>(); // this slice's x column (SoA)
      auto vx = vel.column<0>();
      for (std::size_t i = 0; i < px.size(); ++i)
        px[i] += vx[i];          // tight, vectorizable loop
    });
  });

  WorkerPool pool{8};                            // 8 lanes (1 = plain serial)
  schedule.run(world, pool);                     // parallel rows fan out across lanes
}
```

## Features

| Requirement | How |
|---|---|
| Components defined as a struct | Any aggregate struct; no macros/registration |
| Entities define groups of components | Generational handles; dynamic archetypes (add/remove at runtime) |
| AoS → SoA automatically via reflection | `soa_storage<T>` splits each struct into per-field columns using the reflection facade |
| Cache-friendly layout | Dense per-archetype tables + per-field columns + swap-and-pop |
| Data-parallel execution | A persistent `WorkerPool` (resident, performance-core-pinned, spin-wait, allocation-free dispatch) slices each `add_parallel` system's rows into work items across lanes (a serial `add_serial` system is one opaque item); systems run in dependency order derived from access **declared by their parameter types** (can't drift from actual use), so the split is race-free with no locks |
| System parameters | a system's access comes from its params: `Query<const A, B>` (read A, write B), `Res<T>`/`ResMut<T>` (read/write resource), `Commands&` (deferred mutation), `WorldView` (ad-hoc **read-only** access → runs with readers, after writers). No raw `World&` — unanalyzable access is not a system parameter. No separate `reads<>/writes<>` tags |
| Resources (singletons) | `emplace_resource`/`resource<T>()` for engine services; `Res<T>`/`ResMut<T>` system params track read/write access to them |
| Mutation via `Commands` | `cmd.spawn/destroy/add/remove/set` only record, applied at each schedule wave barrier. `Commands` only exists inside a run (mutation-outside-a-system is a *compile* error) and is non-copyable/non-movable. `spawn` returns a usable handle immediately. Mid-iteration edits are always safe. Commands record into a per-thread monotonic arena, so a steady-state tick records and flushes them **without touching the heap** (zero allocation per tick) |
| Systems: one-shot, removable, phased | `Schedule::add_serial` returns a `SystemId`; `remove(id)` unschedules; a `times` argument retires a system after N runs (`times=1` is one-shot); a `phase<N>` tag orders systems across barriers (e.g. `phase<-1>` startup before normal systems) |
| Snapshot handoff | generic lock-free SPSC `TripleBuffer<T>`, plus `SnapshotChannel<T>` for multi-consumer fan-out, to hand extracted snapshots to consumer threads (renderer/audio/anything) with no tearing or blocking |

## Build

Requires CMake ≥ 3.24 and a C++23 compiler with libstdc++ (the library uses
`std::move_only_function`). No external dependencies — header-only, linking only
the system threads library.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/nbody
./build/benchmarks/soa_bench           # SoA layout microbenchmarks
```

### Benchmarks

`benchmarks/soa_bench` measures the structure-of-arrays storage against a plain
array-of-structures baseline and the two query paths (`for_each` vs
`for_each_chunk`) across representative workloads — integrate, multi
read+write, single-field read, compute-bound, and the write-back cost of an
unmarked read. It is always built `-O3 -march=native`. Override the workload
size with `soa_bench [entities] [iters] [repeats]`. Results are ns per entity
per sweep; the ratios (SoA vs AoS, `const` vs not) are stable run-to-run, the
absolute values track the host's memory subsystem.

### C++26 reflection

By default the library uses a **portable** reflection backend (aggregate arity +
structured bindings) that compiles on stock compilers. To use **real C++26
P2996 static reflection** (`std::meta`) with a toolchain that supports it
(e.g. GCC 16 / `g++-16`, which ships libstdc++):

```sh
cmake -S . -B build -DECS_USE_P2996=ON   # adds -std=c++26 -freflection
```

Both paths implement the same `ecs::reflect` facade, so nothing else changes.

See [DESIGN.md](DESIGN.md) for the architecture.
