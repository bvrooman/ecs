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
// inside a system, given Commands& cmd (see §7):
Entity e = cmd.spawn(Position{0,0,0}, Velocity{1,0,0});     // archetype {P,V}
cmd.add<Frozen>(e);                                         // -> {P,V,Frozen}
cmd.remove<Velocity>(e);                                    // -> {P,Frozen}
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

Queries offer both styles. Mark a component `const` in the query type to read it
without writing it back -- `each` then passes it by `const&` and skips its
scatter (and `for_each_chunk` hands a `const` storage), which avoids both the
cost and the *undeclared write* of touching a component you only read:

```cpp
// ergonomic: gather -> mutate refs -> scatter back the non-const ones
query<Position, const Velocity>(w).each([](Entity, Position& p, const Velocity& v){
  p.x += v.x;   // Velocity is read-only: not written back
});

// fast path: raw columns, tight contiguous loop
query<Position, const Velocity>(w).for_each_chunk(
  [](std::span<Entity>, soa_storage<Position>& pos, const soa_storage<Velocity>& vel){
    auto px = pos.column<0>(); auto vx = vel.column<0>(); // vx is span<const>
    for (size_t i = 0; i < px.size(); ++i) px[i] += vx[i];
  });
```

`each` is the convenience path (per-row gather/scatter + a tuple); prefer
`for_each_chunk` for hot loops.

## 5. Async-runtime compatible (std::execution / P2300)

A *system* is `void(World&, Commands&)` plus the sets of components and resources
it **reads** and **writes** (the callable first, then any number of access tags):

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

### What the scheduler trusts (read this before relying on parallelism)

The conflict analysis is only as correct as the access a system *declares*:

* **Declared access must match actual access.** Nothing checks that a system's
  `query<…>` and resource use line up with its `reads<>/writes<>/reads_res<>/
  writes_res<>` tags. A system that touches a component or resource it did not
  declare can run concurrently with a conflicting system and race. (Marking
  read-only query components `const` — `query<const Position, Velocity>` — helps
  on the component side by preventing the write-back of components you only
  read; see §4. Deriving access from typed system parameters, bevy-style, would
  remove the foot-gun entirely, at the cost of a larger redesign.)
* **Resource access is not gated.** `world.resource<T>()` is reachable from any
  system (and external threads) regardless of declarations; undeclared resource
  mutation is the most likely way to introduce an undetected race.
* **Parallel runs are not deterministic.** Within a wave, command-recording
  order across threads and `reserve()`'s id handout depend on timing, so entity
  ids and creation order vary run to run. Use the inline `run(world)` (or a
  single-threaded scheduler) when you need reproducibility, e.g. lockstep sims.

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

## 7. Mutation through `Commands` (command-buffer-only)

Structural edits -- spawn, destroy, add/remove component -- move data between
archetype tables, which invalidates in-flight iteration and races with systems
running on the schedule's pool; `set` writes a column. So mutation **never
happens directly**: it is recorded into the command buffer and applied only when
flushed. A single uniform rule ("changes apply at the next flush") falls out of
this, and mutating mid-iteration is *always* safe.

Mutation is also unreachable outside a system, enforced **at compile time**:
`World` exposes only reads. The mutators -- `spawn`/`destroy`/`add`/`remove`/
`set` -- live on a `Commands` object that only `Schedule::run` can construct
(bound to the world) and pass to each system. You cannot call `spawn` without a
`Commands`, and you cannot obtain one except from a run, so the "mutate only
inside a system" rule is checked by the type system rather than a runtime assert.
`Commands` is additionally **non-copyable and non-movable**, so it cannot be
squirreled away by value (`auto saved = cmd;` is a compile error); only a raw
pointer/reference can escape, which is a pointer-to-local bug C++ cannot prevent.

```cpp
// a system: World& for reads/queries, Commands& for (deferred) mutation
sched.add("spawn_particles", [](World& w, Commands& cmd) {
  query<Emitter>(w).each([&](Entity, Emitter& em) {
    if (em.fire) {
      Entity p = cmd.spawn(Position{...}, Velocity{...}); // recorded
      cmd.add<Lifetime>(p, {...});                        // edit the handle now
    }
  });
}, reads<Emitter>{});
```

Setup is just a system too -- there is no separate immediate API. Register a
one-shot system and run the schedule (inline, no thread pool needed):

```cpp
Schedule init;
init.add_once("populate", [&](World&, Commands& cmd) {
  for (...) cmd.spawn(Position{...}, Velocity{...});
});
init.run(world);   // inline run on the calling thread
```

The flush points are `Schedule::run`'s wave barriers (so edits from wave *N* are
visible to wave *N+1*) -- the templated `run(world, scheduler)` for the parallel
pool, or `run(world)` for inline single-threaded execution.

The immediate primitives (`*_now`, `materialize`) stay private and are what the
command closures call at flush time. `spawn(...)` reserves an entity handle
immediately and returns it, so a system can record follow-up edits on a
not-yet-created entity in the same frame; reservation is thread-safe via a tiny
locked critical section (new ids come from a monotonic counter so concurrent
reservers never grow `records_` mid-run; reused ids carry the freed slot's
already-bumped generation), and storage is created by `materialize()` at flush.

Bulk creation is just a loop of `spawn()`. Note that every `spawn` records a
closure that is held until the wave flushes, so a single batch of *n* spawns
holds ~one closure per entity transiently (tens of bytes each). That is
inconsequential for per-frame spawning and for setups up to ~1M entities; for a
multi-million-entity *single* batch it can be a meaningful transient spike,
mitigable by splitting setup across several `add_once` runs (each flushes).

Value mutation *through a query* (`each`/`for_each_chunk` writing the component
references it was handed) is not a structural change and is not routed through
the buffer: it is a system writing its own data, which the scheduler has already
proven conflict-free. Only the explicit `world.*` mutators defer.

### One-shot and removable systems

`Schedule::add` returns a `SystemId`; `Schedule::remove(id)` unschedules a
system. `Schedule::add_once(...)` registers a system that runs on the next
`run()` and is then removed -- the idiomatic way to express startup work that
should populate the world once and then stop:

```cpp
Schedule sched;
sched.add_once("startup", setup_fn, phase<-1>{}); // runs before everything, once
sched.add("gameplay", gameplay_fn, ...);           // default phase 0
sched.run(world, scheduler);   // startup runs (and flushes) first, then gameplay
sched.run(world, scheduler);   // only gameplay from here on
```

**Phases** give coarse ordering independent of access conflicts. A `phase<N>`
tag puts a system in phase `N` (default 0); the schedule runs phases in
ascending order with a barrier between them, and within a phase the usual
conflict-based wavefront leveling applies. So `add_once(..., phase<-1>{})` is a
startup phase guaranteed to run -- and have its spawns flushed -- before any
phase-0 system observes the world; `phase<1>` is a teardown/late phase. Without
a phase tag, a one-shot system is leveled by conflicts like any other, which is
often enough but does not guarantee it precedes an unrelated system.

A command is a type-erased `void(World&)` closure (`std::move_only_function`,
so move-only captured values are fine). Recording is **sharded per worker
thread**: shard lookup is a lock-free atomic load and each shard has its own
(normally uncontended) mutex, so many systems on the same level record
concurrently with effectively no contention -- recording is a thread-safe side
channel, not tracked component/resource state. `apply()` drains every shard
single-threaded; commands within one thread's shard replay in record order,
ordering *across* shards is unspecified. The `Schedule` flushes at every level
barrier (no systems running), so changes recorded in level *N* are visible to
level *N+1*; an inline `run(world)` flushes after each wave too. `destroy`/`add`/`remove` no-op
if their target was already destroyed earlier in the same flush, so
order-independent teardown is safe.

`spawn()` itself is contention-light: `World::reserve()` is lock-free on the
growth path (a brand-new index is a single atomic `fetch_add`) and only takes a
short lock to recycle from the free list.

## 8. Snapshot handoff to external threads (triple buffer)

Subsystems that consume world state on their own thread and clock -- a renderer
building draw lists, an audio mixer reading emitter positions, a telemetry sink
-- should not read live component storage while the simulation mutates it. The
pattern is to *extract* a snapshot each frame and hand it across the thread
boundary. `TripleBuffer<T>` is the generic, lock-free mechanism for that.

It is deliberately **agnostic** about what a snapshot is: `T` is any user type
(a `std::vector<DrawItem>`, a struct of arrays, ...). The library has no
rendering/audio knowledge; an extraction system fills `back()` from the SoA
columns (`for_each_chunk`) and calls `publish()`, while the consumer thread
calls `consume()`/`front()`.

```cpp
world.emplace_resource<TripleBuffer<Snapshot>>();          // T is user-defined
sched.add("extract", [](World& w, Commands&){
  auto& tb = w.resource<TripleBuffer<Snapshot>>();
  fill(tb.back(), w);   // copy out of the columns
  tb.publish();
}, reads<Position, Mesh>{});
// ... on the render/audio thread:
if (tb.consume()) draw(tb.front());
```

Three buffers (not two) let the producer and consumer each own a distinct
buffer at all times, so a lagging consumer never reads a buffer the producer is
overwriting -- no tearing, no blocking. The index and the "new data" flag live
in a *single* atomic, so publish and consume are each one indivisible RMW; this
is what makes the SPSC handoff correct (a two-atomic flag/index split races).
Single producer, single consumer; "latest value wins" if the producer outruns
the consumer.

### Fan-out to many consumers: `SnapshotChannel<T>`

When several subsystems need the same snapshot (a renderer *and* an audio
mixer, plus telemetry), `TripleBuffer` -- being strictly single-consumer --
can't serve them. `SnapshotChannel<T>` does: the producer fills `back()` and
`publish()`es it as an immutable `std::shared_ptr<const T>` into an atomic; any
number of consumer threads `load()` it (via a per-consumer `Reader` that
advances only on new data) and hold their own reference while they read.

```cpp
world.emplace_resource<SnapshotChannel<Snapshot>>();
sched.add("extract", [](World& w, Commands&){
  auto& ch = w.resource<SnapshotChannel<Snapshot>>();
  fill(ch.back(), w);
  ch.publish();
}, reads<Position, Mesh>{}, writes_res<SnapshotChannel<Snapshot>>{});
// each consumer thread, independently:
auto r = channel.reader();
while (running) if (r.poll()) consume(*r.get());
```

Because a published snapshot is immutable, all consumers read it concurrently
with no tearing, and a buffer is reclaimed automatically once the producer has
moved on and every consumer has dropped it. The trade vs `TripleBuffer` is an
allocation per publish and reliance on `std::atomic<shared_ptr>` (which the
standard library may back with an internal lock pool rather than true
lock-freedom) -- the price of unlimited consumers. Still single-producer.

The `nbody` example wires the whole thing together: an extract system publishes
tracer positions that a mock "renderer" and "audio" thread both consume.

## Known limitations / next steps

* Portable reflection treats a component as a flat aggregate of ≤32 scalar-ish
  fields; nested aggregates count as one field (P2996 handles these natively).
* `each` gathers/scatters per entity for ergonomic mutation; the column API is
  the zero-copy path. A reflection-generated reference proxy (named field
  accessors under P2996) is a natural follow-up.
* The scheduler uses wavefront leveling (simple, correct, parallel). A full
  per-edge sender DAG would extract a little more overlap across levels.
* All structural mutation is deferred to a flush point, so it is always safe
  (including mid-iteration); the trade is that nothing takes effect until a
  `Schedule` wave barrier, and all mutation must go through a `Commands` (so
  even setup is a one-shot system run via the schedule).
* The command buffer shards over `kShards` (64) lanes; with more concurrent
  worker threads than shards, threads share a shard and contend its mutex (still
  correct, just less scalable).
* Both snapshot primitives are single-*producer*. Multiple producers feeding one
  channel would need external synchronization or a different design.
* `SnapshotChannel` allocates a buffer per publish; a buffer pool that recycles
  snapshots whose last reader has dropped them would remove the steady-state
  allocations.
```
