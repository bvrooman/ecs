# ECS design

A small, header-only Entity-Component-System for C++ whose defining trait is
that **you write components as plain structs (AoS) and they are stored
field-wise (SoA) automatically, using C++ reflection**. Systems run on a
persistent data-parallel worker pool: each system's entities are split across
cores, with ordering and safety derived from the components each system declares
it touches.

```
include/ecs/
  ecs.hpp              umbrella: the whole statically-typed surface
  dynamic.hpp          umbrella: opt-in runtime-described components
  world.hpp            umbrella over world/
  schedule.hpp         umbrella over schedule/

  soa.hpp              soa_storage<T> : AoS API, SoA storage      (the ECS core,
  entity.hpp           generational Entity handle + component_id<T> namespace ecs)
  strong_id.hpp        StrongId<Tag,Rep>, the base of every *Id below
  {archetype,column,component,resource,system,world}_id.hpp  strong id aliases
  archetype.hpp        type-erased component columns grouped by signature
  chunk.hpp            a lane's span-view of one component over a row range
  resource.hpp         type-erased singleton registry
  command_buffer.hpp   deferred structural edits (sharded monotonic arena)
  query.hpp            iterate matching archetypes (ergonomic + SoA paths)

  world/               entity lifecycle + dynamic archetype transitions
    world.hpp            the World itself
    commands.hpp         Commands -- the deferred mutation API systems see
    res.hpp              Res<T> / ResMut<T> resource parameters
  schedule/            conflict analysis + the WorkerPool executor
    access.hpp           phase<N>, SystemAccess, conflicts()
    system.hpp           SystemRecord + the parameter/kernel protocols
    validate.hpp         the consteval checks registration is gated on
    work_item.hpp        WorkItem/Unit -- one claimable slice of a wave
    wave.hpp             the work-item executor (build + dispatch)
    graph.hpp            systems -> wavefront levels -> wave plans
    schedule.hpp         the Schedule class (registration, run loop)
    events.hpp           sched_event::*, ScheduleEvent
    kernel_params.hpp    umbrella over params/
    params/              one header per system parameter (Bin, Collect,
                         Extract, Local, Random, Reduce, Scratch, Events, ...)
  dynamic/             runtime-described components         (namespace ecs::dynamic)
  reflection/          reusable, ECS-agnostic reflection     (namespace ecs::reflect)
  parallel/            the data-parallel runtime            (namespace ecs::parallel)
  sync/                lock-free thread handoff primitives      (namespace ecs::sync)
  event/               Emitter/observer                        (namespace ecs::event)
  detail/              internals                              (namespace ecs::detail)
    container/           generic containers, no ECS concepts
    compat/              toolchain shims (move_only_function, atomic_shared_ptr)
    chunk_fields.hpp, for_each_row*.hpp   ECS-specific, backend-selected

tools/include/ecs/
  diag/                timing observers + sinks                 (namespace ecs::diag)
  viz/                 schedule DAG -> SVG/DOT                   (namespace ecs::viz)
```

The supporting libraries -- reflection, the worker pool, and the snapshot
handoff primitives -- live in their own folders and sub-namespaces (`ecs::reflect`,
`ecs::parallel`, `ecs::sync`) to keep them visibly distinct from the ECS concepts
(`World`, `Entity`, `Query`, `Schedule`) in `ecs`. The developer-facing types
from those layers (`WorkerPool`, `TripleBuffer`, `SnapshotChannel`) are also
re-exported into `ecs` with `using`-declarations, so they remain reachable as
`ecs::WorkerPool` etc.

Four conventions hold across the tree:

- **Includes are rooted.** Every header, in the library and out of it, is
  included as `<ecs/...>` -- never a relative path. An include says what it
  needs, not where the includer happens to sit, so files can move without
  rewriting their dependents.
- **Every header stands alone.** Including any one header compiles on its own;
  none of them lean on a sibling having been included first. (The two P2996
  backends are the exception -- they need a reflection toolchain by
  construction.)
- **A directory that is its own layer gets its own namespace** (`ecs::reflect`,
  `ecs::parallel`, `ecs::sync`, `ecs::event`, `ecs::dynamic`, `ecs::diag`,
  `ecs::viz`). Directories that are an implementation layout for one concept --
  `world/`, `schedule/`, `schedule/params/` -- do not; their contents are the
  core ECS vocabulary and stay in `ecs`.
- **`detail` is spelled to match the file.** A file that is entirely internal
  opens `namespace ecs::detail {`. A file where a public type and its private
  specialization interleave nests `detail` inside `ecs`, rather than closing and
  reopening the outer namespace around each one.

Formatting is `clang-format`, checked in CI:

```
tools/format.sh            # reformat in place
tools/format.sh --check    # what CI runs
```

The version is pinned in `.clang-format-version`, which CI installs and
`tools/format.sh` enforces — one source of truth, so a local run and CI cannot
drift apart. `pip install clang-format==$(cat .clang-format-version)`.

The pin is load-bearing in two directions. Too old and the config does not load
at all: `.clang-format` uses `BinPackParameters: OnePerLine`, an enum only from
clang-format 20, and an older binary fails with a confusing `invalid boolean`
rather than formatting differently. A *different major* simply formats
differently — 20 and 22 disagree about `AlignConsecutiveAssignments` when a
right-hand side wraps, for example. `tools/format.sh` rejects a mismatched major
and tells you which way it is wrong.

Note the formatter and the compiler are deliberately different versions: the
tree builds with clang-18 and formats with the pinned clang-format.

Two spots opt out with `// clang-format off`: the structured-bindings ladder in
`reflection/reflect_portable.hpp`, and the `EM_ASM` bodies in `web/`, which are
JavaScript — clang-format rewrites `!==` into `!= =` there. The marker must be
**exactly** `// clang-format off`; clang-format ignores it if any prose trails
on the same line, which silently unprotects the region.

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

Everything is written against `namespace ecs::reflect`. The shared foundation
both backends build on — the `Reflectable` concept and the `detail::unqualified`
name helper — lives in `reflect_common.hpp`, which each backend includes, so a
backend is a complete, self-contained header (analyzable on its own, not only
once `reflect.hpp` has composed it). The facade (`reflect.hpp`) selects one of
two interchangeable backend headers via `ECS_USE_P2996`, then adds the
backend-independent `for_each_field` on top of the surface that backend defines:

| backend | mechanism | toolchain |
|---|---|---|
| **P2996** (`reflect_p2996.hpp`, `ECS_USE_P2996=1`) | real C++26 static reflection: `^^T`, `nonstatic_data_members_of`, member splicers `[:m:]`, `identifier_of` | GCC 16 (`g++-16` + libstdc++) |
| **portable** (`reflect_portable.hpp`, default) | aggregate brace-arity probe + structured bindings | stock Clang/GCC, C++20/23 |

The P2996 backend is the canonical "C++26 reflection" answer and additionally
recovers real field *names*. The portable backend exists so the engine compiles,
runs, and is tested on today's compilers. Each backend is a self-contained header
implementing the same surface (`field_count_v`, `get_field`, `field_type_t`,
`field_name`, `type_name`); `reflect.hpp` picks exactly one, so switching is a
one-flag change and **no other code moves**. The portable backend supports up to
32 fields per component; P2996 is unbounded.

> Note: real P2996 (`<meta>`, `-freflection`) is still bleeding-edge — it needs
> GCC 16 (`g++-16`) — so the portable backend is the default and what the tests
> here exercise by default.

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

Queries offer two iteration styles -- both pure iterators that never dispatch.
Mark a component `const` in the query type to read it without writing it back --
a read-only component is passed by `const&` and skipped by the write-back (and
`for_each_chunk` hands a `const` chunk), which avoids both the cost and the
*undeclared write* of touching a component you only read:

```cpp
// per-element (ROW-shaped) -- each component by reference, accessed as p.x
query<Position, const Velocity>(w).for_each([](auto& p, auto& v){
  p.x += v.x;   // Velocity is read-only: not written back
});

// per-chunk (COLUMN-shaped, SoA) -- a `chunk` per component (raw column spans)
query<Position, const Velocity>(w).for_each_chunk(
  [](std::span<Entity>, chunk<Position> pos, chunk<const Velocity> vel){
    auto px = pos.column<0>(); auto vx = vel.column<0>(); // vx is span<const>
    for (size_t i = 0; i < px.size(); ++i) px[i] += vx[i];
  });
```

`for_each` is the **row** shape: each entity's components are handed to the kernel
accessed as `p.x` (a write-through proxy referencing the SoA column in place under
P2996; a gathered local on the portable backend), with an optional leading
`Entity`. `for_each_chunk` is the **column** shape: it hands the kernel a `chunk`
per component whose `column<I>()` is that field's contiguous span, so you run a
tight vectorizer-friendly loop over just the fields you touch with **no per-row
gather/scatter** -- prefer it for a wide or sparsely-touched component on the
portable backend, where `for_each` reassembles the whole struct.

Neither form dispatches: both iterate on the calling thread. Parallelism is the
executor's job, not the query's -- an `add_kernel` system slices its matched rows
into work items across the worker pool's lanes, and each item binds a `Query`
restricted to its own `[begin, end)` slice (so a lane physically cannot reach
another's rows: the split is data-parallel and race-free). The same `for_each` /
`for_each_chunk` body then runs over that slice. An imperative `add` system is one
opaque work item, so its query iterates every matched row serially.

The set of archetypes a query matches is **cached** per required-component
signature, so a repeated query costs an O(1) lookup plus iteration of just the
matching archetypes rather than a re-scan of every archetype each call. The
cache is built lazily on first use and extended whenever a new matching
archetype is created; it is mutex-guarded so parallel systems can query
concurrently (the returned match list stays valid after the lock is released
because the cache is append-only and `unordered_map` element references survive
rehash).

## 5. Data-parallel execution

A *system* is a callable whose **parameter types declare what it touches** --
the scheduler derives the read/write sets from them and constructs the
parameters when the system runs, so the declaration cannot drift from the
actual access:

```cpp
Schedule s;
// reads Mass + the Gravity resource, writes Velocity -- all from the params
s.add("gravity",   [](Query<Velocity, const Mass> q, Res<Gravity> g){ ... });
// reads Velocity, writes Position
s.add("integrate", [](Query<Position, const Velocity> q){ ... });
WorkerPool pool{8};        // 8 lanes; 1 = plain serial
s.run(world, pool);        // each system's rows split across the lanes
```

The system parameters are:

| param | access it declares | what it is |
|---|---|---|
| `Query<Cs...>` | each `const C` a read, each non-const `C` a write | iterate matching entities |
| `Res<T>` / `ResMut<T>` | read / write resource T | typed resource handle |
| `Commands&` | none (deferred side channel) | record structural edits |
| `WorldView` | *reads everything* — runs with readers, after writers | ad-hoc **read-only** access |

A trailing `phase<N>` tag is the only non-parameter argument (ordering). A raw
`World&` is deliberately **not** a system parameter: it cannot be analyzed and
is unsafe under any concurrent executor. Setup happens on the `World` directly
(outside a run), mutation goes through `Commands`, ad-hoc reads through
`WorldView`; a system registered via `add_dynamic` may still *declare* itself
exclusive when its access is genuinely unanalyzable.

Conflict analysis: two systems conflict when one's write set intersects the
other's read-or-write set — evaluated independently over the component and
resource id spaces, so a shared mutable resource serializes two systems even
when their component access is disjoint. A `WorldView` (reads-everything) system
conflicts only with writers — two `WorldView` readers still run concurrently —
while a declared-`exclusive` system (`add_dynamic`) conflicts with everything. Each system is assigned a **level** equal to
`1 + max(level)` over earlier conflicting systems; the executor runs waves in
level order, and same-level systems are conflict-free (any order).

Execution (`Schedule::run(world, pool)`) is a **work-item executor**: each wave
is flattened into one list of items and executed with a single fork-join
dispatch on the `WorkerPool`. A **kernel system** — `add_kernel(name, fn)`,
same signature rules as `add()` but with exactly ONE `Query<Cs...>` parameter —
contributes one item per row-range slice of each archetype that query matches
(a fixed target of ~64 slices, floored at 1024 rows — deliberately independent
of the lane count, so the slicing and therefore every barrier-time fold is
identical whether the schedule runs on 1 lane or N): its iteration is *visible* to
the scheduler, which invokes the body once per item with the Query restricted
to that item's rows, so `q.for_each_chunk(...)`/`q.for_each(...)` inside
iterate just the slice, inline on the claiming lane. The other parameters
(`Res<T>`, `Commands&`, `WorldView`, and the per-item primitives — `Reduce<T,
Op>` private partials folded at the barrier, `Extract<T>` disjoint spans over
a pre-sized buffer, `Collect<T>` filtered gather concatenated at the barrier,
`EventWriter<T>`/`EventReader<T>` over a double-buffered `Events<T>` channel,
`Bin<V>` group-by counting-sorted into per-bucket spans of a `Bins<V>`
resource, `Scratch<T>` private workspace, `Random` deterministic
per-item streams) bind per item and fold into the derived access, so a
components-plus-resources system (read the clock, chase a goal, record
spawns, jitter an emitter) keeps slicing — and an imperative system with
independent per-row work becomes a kernel system by changing one word:

```cpp
sched.add_kernel("steer", [](Query<const Position, Velocity> q, Res<Clock> clk) {
  q.for_each([&](auto& p, auto& v) { /* ... */ });
});
```

The body must be independent per-row work — it runs concurrently with the
system's own other items. `ResMut<T>` is therefore rejected in a kernel (the
conflict analysis serializes *other* systems against a resource writer, but a
system's own items run concurrently, so writes through `ResMut` would race
between them); a system that writes a resource is a *reduction* — spelled
`Reduce<T, Op>` (or `Collect`/`Extract` for gather shapes) in a kernel, with
the shared-target writes confined to the single-threaded barrier hooks.
Genuinely ordered iteration still belongs in `add()` systems. An
**imperative system** (`add`/`add_once`/`add_dynamic` — an opaque callable that
may take `Commands&`, resources, `WorldView`, `Local<T>` per-system state
persisting across ticks, do reductions or ordered work)
contributes itself as a single item; its queries are pure iterators that always
walk their matched rows serially on whichever lane claims the item (a
single-system wave runs inline on the caller). Parallelism within a wave comes
only from kernel items -- an imperative system never fans its own rows across
lanes. Items are
sorted longest-first (greedy LPT — the biggest work cannot become the join's
straggler) and claimed by the lanes from an atomic cursor, so a heavy kernel
system's slices overlap both with each other and with the wave's other systems:
data parallelism *within and across* systems from one dispatch, no task queue.
Item contents and order are deterministic; item→lane assignment is not (a
1-lane pool claims in list order and is fully deterministic). Commands recorded
by kernel systems are insulated from that timing: each item records into a
private per-item store, and the barrier enqueues the stores into the wave's
flush in ordinal order, so kernel structural edits *apply* in canonical
serial-walk order at any lane count (`spawn()` still reserves its Entity handle
at record time, so the IDs themselves — not the resulting layout — remain
timing-dependent; imperative systems' commands keep the thread-sharded buffer
with its unspecified cross-shard order). `run(world)` is
sugar for a 1-lane pool. `benchmarks/schedule_bench` quantifies the shape this
buys: a wave of 8 small systems plus one compute-heavy one runs ~2.2× faster
with the heavy system registered as a kernel, because an opaque heavy system is
an unsliceable straggler.

The `WorkerPool` (`parallel/worker_pool.hpp`) is built once and reused
every tick: `lanes` resident threads (the caller is lane 0) stay alive and
spin-wait on a generation counter, so a dispatch never creates, wakes, or sleeps
a thread — the wakeup/barrier latency that hurts a general work-stealing pool.
The cost is that idle lanes keep their cores busy, so an N-lane pool must leave a
core for each other hot thread (a render/main thread) rather than oversubscribe;
*parking* idle lanes to free those cores was tried and measured markedly worse
for a fixed-timestep loop — twice: on a condition variable and again on a
bounded-spin-then-futex hybrid — the per-tick wakeup latency dwarfs the
idle-core cost it saves (so spinning stays). A dispatch is allocation-free (the
kernel is referenced via a static trampoline, not stored) and **not
re-entrant**: there is one job slot, so a nested `parallel_for` on the same pool
throws rather than corrupting the in-flight dispatch (kernel systems cannot
reach this by construction; an imperative system that iterates one pool-bound
query inside another's kernel can). The first exception from any lane is
rethrown on the caller, so a throwing system escapes `run()` as it would
serially. Commands flush at each level barrier. (An earlier
`std::execution`/P2300 backend — a general async framework that parallelized
only *across* systems and woke workers per wave — cost more than it saved for a
tight fixed-timestep loop and was removed; the work-item executor gets the
cross-system overlap without the task-queue overhead.)

### What the scheduler trusts

Because access is *derived from parameter types*, a parallel system cannot read
or write a component/resource it did not declare -- `Query`/`Res`/`ResMut` are
the only way to reach that state, and each contributes its access. The two
remaining caveats:

* **`WorldView` is the read-only escape hatch.** When a system needs ad-hoc
  reads (`size`, `alive`, `has`, `get`, or a `view.query<Cs...>()` that forces
  every component to const) beyond what a single `Query`/`Res` expresses, take a
  `WorldView`. It exposes nothing that can mutate, so it declares "reads
  everything": it still runs concurrently with other readers and is serialized
  only against writers.
* **There is no raw-`World&` parameter.** Unanalyzable read-write access has no
  safe meaning under a concurrent executor, so the type system simply does not
  offer it; `add_dynamic` can declare a system `exclusive` when a runtime-typed
  system is genuinely unanalyzable, and it then runs alone. (Resource reads from
  outside any system, e.g. a consumer thread calling `world.resource<T>()`,
  remain the caller's responsibility.)
* **Commands recorded from a parallel kernel are unordered.** Structural edits
  recorded *inside* a `for_each_chunk` kernel land in per-lane shards whose
  cross-lane order is unspecified, and `reserve()`'s id handout then depends on
  timing. Record structural edits from a system's serial part instead (the common
  case — an emitter that loops and `cmd.spawn()`s, a reaper using
  `for_each`), or use
  a 1-lane pool, when you need reproducibility, e.g. lockstep sims.

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
systems are safe; the scheduler serializes any system with a `ResMut<T>`
parameter against others touching the same resource.

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
// a system: a Query param (here read-only Emitter) + Commands for mutation
sched.add("spawn_particles", [](Query<const Emitter> q, Commands& cmd) {
  q.for_each([&](auto& em) {
    if (em.fire) {
      Entity p = cmd.spawn(Position{...}, Velocity{...}); // recorded
      cmd.add<Lifetime>(p, {...});                        // edit the handle now
    }
  });
});
```

Setup is just a system too -- there is no separate immediate API. Register a
one-shot system and run the schedule (inline, no thread pool needed):

```cpp
Schedule init;
init.add_once("populate", [&](Commands& cmd) {
  for (...) cmd.spawn(Position{...}, Velocity{...});
});
init.run(world);   // inline run on the calling thread
```

The flush points are `Schedule::run`'s wave barriers (so edits from wave *N* are
visible to wave *N+1*) -- `run(world, pool)` runs each system data-parallel across
the pool's lanes; `run(world)` is the same on a 1-lane (serial) pool.

The immediate primitives (`*_now`, `spawn_now`) stay private and are what the
command closures call at flush time. `spawn(...)` reserves an entity handle
immediately and returns it, so a system can record follow-up edits on a
not-yet-created entity in the same frame; reservation is thread-safe via a tiny
locked critical section (new ids come from a monotonic counter so concurrent
reservers never grow `records_` mid-run; reused ids carry the freed slot's
already-bumped generation). At flush, `spawn_now` places the entity **directly**
into its final archetype -- pushing all of its components in one pass rather
than walking it through one archetype transition per component (which would also
leave empty intermediate archetypes behind for queries to scan). Adding a
component to an *existing* entity (`add`) still relocates, since that genuinely
moves it between archetypes.

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
sched.run(world, pool);   // startup runs (and flushes) first, then gameplay
sched.run(world, pool);   // only gameplay from here on
```

**Phases** give coarse ordering independent of access conflicts. A `phase<N>`
tag puts a system in phase `N` (default 0); the schedule runs phases in
ascending order with a barrier between them, and within a phase the usual
conflict-based wavefront leveling applies. So `add_once(..., phase<-1>{})` is a
startup phase guaranteed to run -- and have its spawns flushed -- before any
phase-0 system observes the world; `phase<1>` is a teardown/late phase. Without
a phase tag, a one-shot system is leveled by conflicts like any other, which is
often enough but does not guarantee it precedes an unrelated system.

A command is a type-erased `void(World&)` callable. Recording is **sharded per
worker thread**: shard lookup is a lock-free atomic load and each shard has its
own (normally uncontended) mutex, so many systems on the same level record
concurrently with effectively no contention -- recording is a thread-safe side
channel, not tracked component/resource state. Each shard keeps its callables in
a **monotonic arena**: the callable is constructed in place in a chain of fixed
blocks and never relocated (so a move-only or non-trivially-relocatable capture
is safe), and `apply()` invokes then destroys each in record order and *rewinds*
the arena rather than freeing it. With each spawn's archetype signature also
computed once and cached, a steady-state tick records and flushes its commands
**without touching the heap** -- zero allocation per tick, where a
`std::move_only_function` with a large multi-component spawn capture would
otherwise `malloc` the closure on every call. `apply()` drains every shard
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
sched.add("extract", [](Query<const Position, const Mesh> q,
                        ResMut<TripleBuffer<Snapshot>> tb){
  fill(tb->back(), q);   // copy out of the columns
  tb->publish();
});
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
sched.add("extract", [](Query<const Position, const Mesh> q,
                        ResMut<SnapshotChannel<Snapshot>> ch){
  fill(ch->back(), q);
  ch->publish();
});
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
* `for_each` hands each entity a reflection-generated reference proxy -- named
  field accessors (`p.x`) that write through to the SoA column in place under
  P2996, with a gather/scatter fallback on the portable backend;
  `for_each_chunk`'s column API is the raw zero-copy path.
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
