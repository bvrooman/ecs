# ecs

A header-only **Entity-Component-System** for modern C++ where you write
components as plain structs (array-of-structures) and they are stored
**structure-of-arrays automatically, using C++ reflection** — laid out for
cache-friendly, vectorizable iteration and scheduled on a `std::execution`
(P2300) async runtime.

```cpp
#include "ecs/ecs.hpp"
#include <exec/static_thread_pool.hpp>
using namespace ecs;

struct Position { float x, y, z; };   // components are just structs
struct Velocity { float x, y, z; };

int main() {
  World world;
  for (int i = 0; i < 100'000; ++i)            // an entity is whatever
    world.create_with(Position{}, Velocity{1, 0, 0}); // components you give it

  exec::static_thread_pool pool{8};
  Schedule schedule;
  // A system: its callable, then the components/resources it reads & writes.
  schedule.add("integrate",
               [](World& w) {
                 query<Position, Velocity>(w).for_each_chunk(
                     [](std::span<Entity>, soa_storage<Position>& pos,
                        soa_storage<Velocity>& vel) {
                       auto px = pos.column<0>(); // contiguous x column (SoA)
                       auto vx = vel.column<0>();
                       for (std::size_t i = 0; i < px.size(); ++i)
                         px[i] += vx[i];          // tight, vectorizable loop
                     });
               },
               reads<Velocity>{}, writes<Position>{});

  schedule.run(world, pool.get_scheduler());     // runs on the async runtime
}
```

## Features

| Requirement | How |
|---|---|
| Components defined as a struct | Any aggregate struct; no macros/registration |
| Entities define groups of components | Generational handles; dynamic archetypes (add/remove at runtime) |
| AoS → SoA automatically via reflection | `soa_storage<T>` splits each struct into per-field columns using the reflection facade |
| Cache-friendly layout | Dense per-archetype tables + per-field columns + swap-and-pop |
| Async-runtime compatible | `std::execution`/P2300 scheduler with read/write conflict analysis |
| Resources (singletons) | `emplace_resource`/`resource<T>()` for engine services; `reads_res`/`writes_res` extend conflict analysis to them |
| Deferred structural edits | `world.commands().spawn/destroy/add/remove`; thread-safe recording, applied single-threaded at each schedule level barrier |

## Build

Requires CMake ≥ 3.24 and a C++23 compiler (tested with Clang 18). The P2300
backend (NVIDIA `stdexec`) is fetched automatically.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/examples/nbody
```

### C++26 reflection

By default the library uses a **portable** reflection backend (aggregate arity +
structured bindings) that compiles on stock compilers. To use **real C++26
P2996 static reflection** (`std::meta`) with an experimental toolchain such as
the clang-p2996 fork:

```sh
cmake -S . -B build -DECS_USE_P2996=ON   # adds -std=c++26 -freflection
```

Both paths implement the same `ecs::reflect` facade, so nothing else changes.

See [DESIGN.md](DESIGN.md) for the architecture.
