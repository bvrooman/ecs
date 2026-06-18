# ECS design

A small, header-only Entity-Component-System for C++ whose defining trait is
that **you write components as plain structs (AoS) and they are stored
field-wise (SoA) automatically, using C++ reflection**. Systems are scheduled on
a `std::execution` (P2300) runtime, so independent work runs in parallel on
whatever async runtime the application already uses.

```
include/ecs/
  reflect.hpp    field reflection facade (P2996 backend + portable backend)
  soa.hpp        soa_storage<T> : AoS API, SoA storage
  entity.hpp     generational Entity handle, ComponentId
  archetype.hpp  type-erased component columns grouped by signature
  world.hpp      entity lifecycle + dynamic archetype transitions
  query.hpp      iterate matching archetypes (ergonomic + SoA fast path)
  schedule.hpp   std::execution system scheduler with conflict analysis
  ecs.hpp        umbrella header
```

## 1. Components are structs

A component is any aggregate struct. No base class, no macros, no registration:

```cpp
struct Position { float x, y, z; };
struct Velocity { float x, y, z; };
struct Frozen {};            // a zero-field "tag" component
```

## 2. Entities define groups of components

An entity is a generational handle (`{index, generation}`). It "defines a group
of components" purely by which components it holds; that set is its
**archetype**. Components can be added and removed at runtime, which moves the
entity between archetype tables (dynamic archetypes, like flecs/EnTT):

```cpp
World w;
Entity e = w.create_with(Position{0,0,0}, Velocity{1,0,0}); // archetype {P,V}
w.add<Frozen>(e);                                           // -> archetype {P,V,Frozen}
w.remove<Velocity>(e);                                      // -> archetype {P,Frozen}
```

The generation counter invalidates stale handles: when an entity is destroyed
its slot's generation is bumped, so a previously copied `Entity` no longer
compares equal to whatever later reuses the slot.

## 3. AoS written, SoA stored, via reflection

`soa_storage<T>` is the heart of the library. Given a component `T`, reflection
enumerates its data members at compile time and the storage keeps **one
contiguous column per field** rather than an array of `T`:

```
soa_storage<Position>           // logically Position[]
  column<0> -> [x0 x1 x2 ...]   // physically three separate arrays
  column<1> -> [y0 y1 y2 ...]
  column<2> -> [z0 z1 z2 ...]
```

* `push_back(const T&)` *scatters* a value's fields into their columns.
* `gather(row) -> T` *reassembles* one element (the AoS view).
* `column<I>()` returns a `std::span` over one field across all elements — the
  cache- and SIMD-friendly access pattern that motivates SoA.

This is implemented once, generically, in terms of the reflection facade
(`field_count_v`, `get_field<I>`, `field_type_t`), so it works for any
component without per-type boilerplate.

### Reflection: one facade, two backends

Everything is written against `namespace ecs::reflect`. There are two
interchangeable implementations, selected by `ECS_USE_P2996`:

| backend | mechanism | toolchain |
|---|---|---|
| **P2996** (`ECS_USE_P2996=1`) | real C++26 static reflection: `^^T`, `nonstatic_data_members_of`, member splicers `[:m:]`, `identifier_of` | experimental (clang-p2996 / EDG) |
| **portable** (default) | aggregate brace-arity probe + structured bindings | stock Clang/GCC, C++20/23 |

The P2996 backend is the canonical "C++26 reflection" answer and additionally
recovers real field *names*. The portable backend exists so the engine compiles,
runs, and is tested on today's compilers. Because both implement the same facade,
switching is a one-flag change and **no other code moves**. The portable backend
supports up to 32 fields per component; P2996 is unbounded.

> Note: real P2996 (`<meta>`, `-freflection`) is not in any released compiler
> yet, which is why the portable backend is the default and what the CI/tests
> here actually exercise.

## 4. Cache-friendly memory layout

Two levels of contiguity maximise locality:

1. **Per archetype**, entities that share a component set are stored together,
   so iterating a query touches dense arrays with no holes.
2. **Per component**, fields are split into separate columns
   (`soa_storage<T>`), so a system that only uses one field streams just that
   field through cache and the loop auto-vectorizes.

Structural edits use swap-and-pop to keep every column dense, patching the
relocated entity's row record in O(1). Archetype objects are heap-allocated so
their addresses stay stable as new archetypes appear.

Queries offer both styles:

```cpp
// ergonomic: gather -> mutate refs -> scatter back
query<Position, Velocity>(w).each([](Entity, Position& p, Velocity& v){
  p.x += v.x;
});

// fast path: raw columns, tight contiguous loop
query<Position, Velocity>(w).for_each_chunk(
  [](std::span<Entity>, soa_storage<Position>& pos, soa_storage<Velocity>& vel){
    auto px = pos.column<0>(); auto vx = vel.column<0>();
    for (size_t i = 0; i < px.size(); ++i) px[i] += vx[i];
  });
```

## 5. Async-runtime compatible (std::execution / P2300)

A *system* is `void(World&)` plus the sets of components and resources it
**reads** and **writes** (the callable first, then any number of access tags):

```cpp
Schedule s;
s.add("gravity",   gravity_fn,   reads<Mass>{},     writes<Velocity>{},
                                 reads_res<Gravity>{});
s.add("integrate", integrate_fn, reads<Velocity>{}, writes<Position>{});
s.run(world, scheduler);   // scheduler is any P2300 scheduler
```

Conflict analysis: two systems conflict when one's write set intersects the
other's read-or-write set — evaluated independently over the component id space
and the resource id space, so a shared mutable resource serializes two systems
even when their component access is disjoint. Each system is assigned a
**level** equal to `1 + max(level)` over earlier-registered conflicting systems.
Same-level systems are provably conflict-free and therefore run concurrently;
levels are separated by a barrier.

Execution is pure senders/receivers: each system becomes
`starts_on(scheduler, then(just(), run))`, spawned into an
`exec::async_scope`; each level barrier is `sync_wait(scope.on_empty())`. Any
P2300 scheduler plugs in — `exec::static_thread_pool` here, but equally a GPU
scheduler or an Asio-backed one — which is what "async-runtime compatible"
means in practice. The reference P2300 implementation (NVIDIA `stdexec`) is
pulled in via CMake `FetchContent`.

## 6. Resources (singletons)

Not all state belongs to an entity. Engine services (renderer, audio engine,
physics world), the frame clock, command buffers and config are **resources**:
exactly one instance of a type, borrowed by reference.

```cpp
world.emplace_resource<Gravity>(-9.81f);   // construct in place
float g = world.resource<Gravity>().accel; // borrow inside a system
```

Resources are stored type-erased in a `ResourceRegistry` (one `unique_ptr` per
type, keyed by a `resource_id<T>` in a separate id space from components). They
are never required to be copyable, so move-only services (a GPU device, an audio
stream) store directly. The registry is set up before a schedule runs and is not
mutated during execution, so concurrent `resource<T>()` borrows from parallel
systems are safe; the scheduler serializes any system declaring `writes_res<T>`
against others touching the same resource.

## Known limitations / next steps

* Portable reflection treats a component as a flat aggregate of ≤32 scalar-ish
  fields; nested aggregates count as one field (P2996 handles these natively).
* `each` gathers/scatters per entity for ergonomic mutation; the column API is
  the zero-copy path. A reflection-generated reference proxy (named field
  accessors under P2996) is a natural follow-up.
* The scheduler uses wavefront leveling (simple, correct, parallel). A full
  per-edge sender DAG would extract a little more overlap across levels.
* Single-threaded structural edits: don't add/remove components while iterating
  a query.
```
