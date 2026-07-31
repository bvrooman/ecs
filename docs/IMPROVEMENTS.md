# Improvement roadmap

A consolidated audit of the library, tests, benchmarks, examples, tooling, and
web layer (July 2026). Items marked **[done]** were implemented on the branch
that introduced this document; the rest are recorded as a roadmap.

## Top ten highest-leverage actions

1. **[done]** Fix the exception-safety hole family in the command/flush path —
   a throwing command replayed the entire shard on the next flush, and a
   mid-wave throw left stale commands that a *different* schedule would
   silently apply (`command_buffer.hpp`, `schedule.hpp`).
2. **[done]** Guard `WorkerPool` against nested dispatch — a `Query` used
   inside a chunk kernel clobbered the shared job slot (silent corruption or
   deadlock); a nested dispatch now throws `std::logic_error` (disallowed by
   design; the work-item executor avoids nesting structurally — kernel systems
   hold no pool at all, and imperative items bind a 1-lane pool).
3. **[done]** Fix the native/dynamic column type confusion — `register_native<T>`
   made `Registry::is_dynamic()` lie, and `WorldOps` then `static_cast`ed a
   `Column<T>` to `DynamicColumn`: UB reachable from the JS host path.
4. **[done]** Add CI with sanitizers — ASAN/UBSAN + TSAN legs, gcc + clang,
   Debug + Release (`.github/workflows/ci.yml`, `ECS_SANITIZE` option).
5. **[done]** Add multi-lane tests for the two headline concurrent paths —
   `for_each_chunk`/`for_each_parallel` splitting and sharded command
   recording previously ran serial-only in the entire suite (every query test
   was below the 256-row parallel threshold).
6. **[done]** Cross-system parallelism, via the **work-item executor**: each
   wave is flattened into one LPT-sorted item list — declarative kernel
   systems (`add_parallel<Cs...>`) contribute row-range slices, imperative
   systems one opaque item each — executed with a single dispatch, lanes
   claiming items dynamically. Replaces the interim opt-in `RunPolicy`;
   `benchmarks/schedule_bench` shows ~2.2x on a mixed heavy+small wave.
   (See DESIGN.md §5; a full DAG/job-graph executor remains roadmap and this
   item model is its foundation.)
7. Query filters (`Without`/`With`/`Optional`) and change detection
   (`Changed`/`Added`) — the two most-missed ECS features vs bevy/flecs; both
   fit this design cheaply (see §6).
8. **[done]** Archetype edge cache + flat sorted columns — `add`/`remove`
   heap-allocated and hashed a signature per structural op, and every column
   lookup paid an `unordered_map` chase.
9. **[done]** Wasm: `-msimd128`, `-fwasm-exceptions`, and the f32-only
   typed-array bug in `defineParallelSystem`. (Idle-lane parking was
   prototyped and benchmarked for the Worker-burn problem but rejected — see
   §2; an explicit app-driven idle API is the roadmap answer.)
10. Install/packaging + a named-module target — no install rules, no version,
    no `find_package` support today; the module plan (§5) is mostly mechanical
    once that lands.

---

## 1. Correctness

### Command buffer & flush
- **[done]** `CommandStore::apply` ran all invokes, then all destroys, then
  cleared; a throwing command left the shard intact and the next flush
  **re-executed every command from the beginning** (duplicate spawns of the
  same reserved handle, double destroys). Now invokes-and-destroys in one pass
  with unwind cleanup that destroys the remaining commands and rewinds.
- **[done]** A system throwing mid-wave propagated out of `Schedule::run`, but
  the world-owned buffer kept earlier systems' recorded edits; the **next**
  run (possibly a different schedule) silently applied them, and
  `spawn()`-reserved ids leaked. `Schedule::run` now discards pending commands
  on unwind (`CommandBuffer::discard()`).
- **[done]** `soa_storage::push_back` scattered field-by-field; a throw at
  column *k* permanently desynchronized the columns (silent data corruption).
  Same class of bug in `World::relocate`. Columns are now pre-reserved so the
  scatter cannot throw mid-way for nothrow-copyable fields.

### Concurrency
- **[done]** Nested `parallel_for` overwrote the pool's single job slot while
  lanes were mid-flight; now detected and rejected with `std::logic_error`
  (compile-time prevention is not expressible — lambda captures are opaque to
  the type system — so the sanctioned nested context, the work-item executor,
  avoids nesting structurally instead: kernel systems hold no pool, imperative
  items bind a 1-lane one).
- **[done]** `next_component_id()` / `next_resource_id()` were plain
  `counter++` on shared statics — two types first-touched on two threads could
  get the same id, silently aliasing columns and merging conflict analysis.
  Now atomic.
- **[done]** `CommandBuffer::apply` drained shards without taking shard
  mutexes (safe only by invariant); it now locks each (uncontended) mutex.

### Type-system / API integrity
- **[done]** `Query<Position, const Position>` compiled and handed a kernel two
  aliased chunks, one mutable; now a named `static_assert`.
- **[done]** Non-const `World::archetypes()` handed any `World&` system full
  structural mutation, bypassing the Commands-only invariant; now const-only.
- **[done]** `make_archetype` published the `sig_index_` entry before pushing
  the archetype — a throwing `push_back` left a dangling index.
- **[done]** `swap_remove` self-move-assigned when removing the last row and
  indexed wildly on an empty store; both guarded (native + dynamic columns).
- **[done]** Dynamic layer: descriptor redefinition invalidated live
  `DynamicColumn`s (now rejected); `register_native` validates blob layout
  (`sizeof(T) == stride`) so padded structs can't be silently misread.
- **[done]** Portable reflection: C-array members and aggregates with base
  classes now produce named `static_assert`s instead of a structured-binding
  ladder explosion; `field_name<I>` returns unique `"field0"…` names.
- **[done]** `Entity` gained a null sentinel (`Entity::null()`,
  `explicit operator bool`) and `operator<=>`.
- **[done]** Missing `<cstdint>` includes; debug asserts in `chunk`,
  `field_base`, `move_row_to`; `Emitter` re-entrancy debug assert;
  `soa_storage::column<I>()` constrained against rvalues.
- Cross-thread reads of `records_`/`alive_count_` from consumer threads (the
  render-thread `alive(e)` pattern) are wave-scoped only — documentation gap.

## 2. Performance

### Scheduling & parallelism
- **[done]** Cross-system parallelism via the work-item executor (see item 6
  above); `RunPolicy` was interim and removed.
- **[done]** `Schedule::prewarm(World&)`: pre-pays a kernel wave's one-time
  first-tick state sizing (per-item slot arrays + prepare hooks: reduce
  targets reset, extract/collect/bin targets pre-size) against the populated
  world, so the first real tick allocates nothing and its per-system busy
  times are steady from tick 0 instead of a cold outlier. Surfaced by the
  schedule report: mist's `grid` first tick fell from 3.9x to 1.16x steady
  (whole tick 2.45x -> 1.00x). Partials with fixed-size scratch (mist's
  `CellAccum` dense index) allocate it in their constructor so slot sizing
  pre-pays it. Runs no bodies/folds -> non-mutating; verified bitwise-equal
  to an un-prewarmed run. The load-then-prewarm-then-loop pattern is the
  standard engine answer (pre-allocate at load, keep the frame allocation-
  free); see examples/mist/tools/headless.cpp (`ECS_PREWARM=0` A/Bs it).
- **[done]** `for_each_chunk` paid one fork-join barrier per archetype and
  applied the 256-row serial threshold per archetype; now a single flattened
  `parallel_for` over the total row count, mapping global ranges to
  (archetype, row-range) segments.
- **[re-evaluated: rejected]** Idle-lane parking. A bounded-spin-then-futex
  (`atomic::wait`) hybrid was implemented and benchmarked against pure
  spinning: any budget that actually parks between 60–120 Hz ticks multiplied
  dispatch p99 by 5–100× on a contended host (matching the DESIGN.md condvar
  measurement), so the pool spins unconditionally. The idle-burn problem
  (paused sims, Web Workers pinned at 100%) should be solved by the app
  explicitly — destroy or stop dispatching to a pool whose work is gone; an
  explicit `park()`/`resume()` API is a roadmap candidate.
- **[done]** Query match lookup took a global mutex and hashed a signature
  vector on every `for_each_*`; now memoized per query type against a world
  archetype-generation counter (steady state: one relaxed load).
- **[done]** The AD-HOC read path (`World::count/for_each/for_each_chunk`, which
  `WorldView` and `ColumnView` forward to) kept paying that cost: it built a
  `Signature` — a heap-allocating vector — and took the query-cache mutex on
  every call. A gather-shaped parallel body calls it once per WORK ITEM, so it
  sat on the hot path. The signature is now a per-instantiation static and the
  match list is memoized per (world, archetype generation) in a thread_local:
  26 ns -> 3 ns per call (see `docs/FMM_FEASIBILITY.md` §3.4).
- **[done]** `rebuild()` captured an id vector **by value** per `intersects`
  call, used O(n²·|ids|²) linear scans and a per-rebuild `std::map`; now
  by-reference, sorted-merge intersection, and flat vectors.
- Static equal split has no load balancing for skewed kernels — a deterministic
  chunk-claiming scheme (lanes × 4–8 chunks via one atomic `fetch_add`) keeps
  chunk contents deterministic while letting lane assignment float. Roadmap.

### Storage
- **[done]** Archetype add/remove **edge cache** — steady-state transitions are
  one array lookup, zero allocation (previously a fresh heap `Signature` +
  hash + map lookup per structural op).
- **[done]** `Archetype::columns` flattened from
  `unordered_map<ComponentId, unique_ptr<IColumn>>` to a vector parallel to
  the sorted signature; `relocate` is now a two-pointer merge.
- **[done]** Move path: `push_back(T&&)`, move-aware `move_row_to`, and
  forwarding `Commands::spawn/add/set` — heap-owning fields no longer pay a
  full copy per archetype transition.
- **[done]** `World::reserve_entities(n)` + `Archetype::reserve(n)` for batch
  spawns; `Record` packed from 16 to 12 bytes (alive bit in the generation).
- **[done]** Entity recycling no longer takes a mutex in the common case
  (atomic cursor over the flush-frozen free list).
- **[done]** False-sharing padding for the pool's `gen_`/`remaining_` and
  `TripleBuffer`; `SnapshotChannel` recycles publish buffers via a
  `use_count()==1` free list.
- Tag components still allocate one virtual column per archetype (LOW);
  a shared singleton tag column is roadmap.
- Chunked storage (Unity-DOTS style fixed blocks) is deliberately **not**
  adopted: dense per-field columns maximize contiguity/SIMD; instead prefer
  `reserve` + a column version counter for host-pointer invalidation.

## 3. Ergonomics (roadmap)

- `Res<T>` from `World const&`; `try_get<C>(Entity) -> C const*` /
  `std::optional<C>`; a consistent `std::expected<T, ecs::errc>` surface
  (would also clean up `WorldOps`' bool-plus-out-param style).
- `SystemParam`/`SystemFn` concepts so unsupported system signatures produce a
  named diagnostic (the `fn_traits` ladder rejects generic lambdas,
  ref-qualified `operator()`, and function references with opaque errors).
- `std::formatter<Entity>`; `alignas(32)` components in `cmd.spawn` (arena
  currently hard-errors); document `World` immovability.

## 4. Modern C++ (C++26) (roadmap)

- `template for` (P1306) to collapse every `index_sequence` IIFE — start with
  the P2996-only files (zero portability cost).
- P2996 annotations (P3394): `[[=ecs::skip]]`, `[[=ecs::name("...")]]` to
  drive SoA splitting and JS interchange names; auto-derive
  `register_native<T>()` names from `field_name`; generate `parse_type` from
  `enumerators_of(^^FieldType)`; reflect over `^^chunk` instead of the
  hardcoded collision list.
- Stable ids: `stable_component_id<T> = fnv1a(type_name<T>())` for
  serialization/interop; a reflection-driven `ecs/serialize.hpp`.
- `std::inplace_vector<ComponentId, N>` for `Signature`; `std::jthread` +
  `stop_token` in `WorkerPool`; `std::flat_map` in `rebuild()`;
  `std::function_ref` for factory params; `std::bit_cast` in
  `std::hash<Entity>`; `[[assume]]` in the hot paths; C++26
  `<hazard_pointer>` as an optional `SnapshotChannel` backend.
- Polyfills (`move_only_function`, `atomic_shared_ptr`) are correctly
  feature-gated; the blocker is Emscripten's libc++ — recheck per emsdk
  upgrade. Do **not** adopt `std::hive` for storage (stable addresses defeat
  the SoA premise).
- Fix the facade divergence: the two reflection backends disagree on
  `constexpr` vs `consteval` for `field_name`/`type_name`.

## 5. C++ modules & packaging (roadmap)

Ship a thin module interface unit wrapping the headers (the fmt pattern):
`src/ecs.cppm` with the std includes in the global module fragment, then
`export module ecs;` + `#define ECS_EXPORT export` + `#include "ecs/ecs.hpp"`.

Blockers to clear first:
- Presets pin `Unix Makefiles` (modules need Ninja); `cmake_minimum_required`
  3.24 → 3.28 for `FILE_SET CXX_MODULES`.
- Config macros (`ECS_USE_P2996`, `ECS_REFLECT_NAMES`) need an always-textual
  `config.hpp` + `ecs::config` constants for `if constexpr` in importers.
- **Hazard**: inline-function singletons (`next_component_id`,
  `dynamic::registry()`, `detail::serial_pool()`, name maps) mean mixing
  `#include` and `import` TUs yields two id counters → colliding component
  ids. Move them into a small non-inline TU in the module target.
- Expect Clang-first (GCC modules are shakiest with heavy templates, which
  cuts across the GCC-16 P2996 preset); keep wasm on headers.

Packaging (prerequisite, currently absent): `install(TARGETS ... FILE_SET
HEADERS)`, `install(EXPORT)` + `ecsConfig.cmake` (with
`find_dependency(Threads)`), `project(... VERSION)`, options gated on
`PROJECT_IS_TOP_LEVEL`, `cxx_std_26` on the interface when P2996 is on,
`-freflection` guarded by compiler id, `tools/` INTERFACE targets need
`BUILD_INTERFACE` guards, then a trivial `vcpkg.json`.

## 6. ECS feature gaps vs bevy_ecs / flecs / EnTT (roadmap)

0. **[done]** Cross-row reads with precise access — `ColumnView<Cs...>`
   (`world/column_view.hpp`): every matching archetype's FULL columns,
   read-only, declaring a read on exactly `Cs...`. `WorldView` already permitted
   the access but declared "reads everything", which serializes a gather-shaped
   system against every writer in its phase; a schedule built out of gathers (a
   tree pass, a solver reading its neighbours) collapsed into one-system waves,
   or left the model entirely via a resource holding raw pointers. Because a
   view matches every archetype *containing* its components, a tag narrows it,
   which is how a cross-row read is kept provably disjoint from the system's own
   writes. Also `chunk::row_begin()`, so a body can place its slice.
   `docs/FMM_FEASIBILITY.md` §3.1/§3.3 has the measured before/after.
1. `Without<C>` / `With<C>` / `Optional<C>` query filters — `Without` is
   nearly free (extend the match-cache key with an excluded set); `Optional`
   is a nullable per-archetype chunk.
2. Change detection — coarse per-(archetype, column) ticks bumped at dispatch
   time for non-const query components unlock `Changed<C>`/`Added<C>`.
3. Frame-buffered gameplay events — **done** (see item 7): `Events<T>`
   channel resource, double-buffered per `run()`, written through per-item
   slots (not shards — deterministic order), declared via
   `EventReader<T>`/`EventWriter<T>` kernel params so conflict analysis
   covers them (`event::Emitter` is a synchronous instrumentation bus, not
   this).
4. `run_if` conditions and explicit `before/after` ordering (today only
   registration order and `phase{n}`).
5. Immediate edits for exclusive systems — an `ExclusiveWorld` param exposing
   the `*_now` primitives. (The raw `World&` parameter has since been removed
   outright — unanalyzable access is not a system parameter; `ExclusiveWorld`
   is the roadmap replacement for genuinely exclusive work.)
6. Batch spawning (`spawn_batch(n, factory)` — fixes the closure-per-spawn
   spike), ~~`Local<T>` per-system state~~ (**done** — see item 7),
   ~~per-dispatch grain hints~~ (**done** — the `grain{n}` registration option
   sets a parallel system's rows per work item outright, replacing the default
   sizing. That default is a policy for cheap per-row work and a heavy kernel
   breaks it both ways: below the 1024-row floor it becomes one item — one lane
   at any lane count — and above the ~64-item target its longest item becomes
   the straggler every lane waits on. `docs/FMM_FEASIBILITY.md` §3.2/§5.1
   measures both), a thin
   `App`/fixed-timestep layer; larger: structural-change hooks, entity
   relationships.
7. **The parallel-primitives suite.** Nearly every safe cross-item pattern a
   kernel system needs is ONE mechanism — per-item private slots plus an
   optional deterministic barrier-time merge — wearing different typed faces.
   Organized by where per-item state lives, whether output size is known, and
   how it merges:

   | primitive | shape | output size | merge | status |
   |---|---|---|---|---|
   | kernel map | write own components in place | — | none (disjoint rows) | done |
   | `Commands&` | structural edits | unknown | kernel systems: per-item stores enqueued at the barrier in ordinal order (canonical replay); imperative systems: sharded flush | **done** |
   | `Reduce<T, Op, Partial = T>` | fold state (sums, bounds, grids); `Partial` may differ from the target — e.g. a sparse map of touched cells folded into a big dense grid | small | barrier fold, canonical order | **done** |
   | `Extract<T>` | gather where output ≈ rows | known | none (disjoint scatter at known offsets) | **done** |
   | `Collect<T>` | filtered gather (kill/visibility/target lists) | unknown | barrier concat, canonical order | **done** |
   | `EventWriter/Reader<T>` | messages between systems (item 3 above; `Events<T>` channel resource, tick-keyed buffer swap, serial systems participate via `Res`/`ResMut<Events<T>>`) | unknown | barrier concat into double-buffered channel, swap once per tick | **done** — same slot plumbing as Collect |
   | `Scratch<T>` | per-item temp workspace (neighbor lists) | — | none; reset keeping capacity | **done** |
   | `Random` | per-item deterministic RNG stream (counter-based PCG32, seeded by `RandomSeed` resource/tick/system/item — repairs the `ResMut<Rng>` ban casualty with BETTER determinism than the serial version) | — | none | **done** |
   | `Local<T>` | per-SYSTEM state across ticks (item 6 above; imperative systems only — rejected in `add_parallel`, where per-item state is `Scratch` and merging state is `Reduce`/`Collect`) | — | none | **done** |
   | `Bin<V>` / group-by | (bucket, value) emission into contiguous per-bucket spans of a `Bins<V>` resource (spatial hashing) | buckets | barrier counting sort: count → prefix-sum → ordinal-order scatter | **done** |

   **The unifying prize — lane-count invariance.** Every primitive merges in
   canonical world order, so a schedule's observable results are bitwise
   identical at 1 lane and N lanes (including float reductions) — the property
   lockstep/replay systems need and per-row atomics can never give. The
   capstone landed too: kernel-recorded `Commands` route through per-item
   stores enqueued at the barrier in ordinal order, so kernel structural
   edits *apply* in canonical order (same rows, same swap-and-pop layout at
   any lane count). What remains nondeterministic, deliberately documented:
   `spawn()` reserves its Entity handle at record time (the API returns a
   usable handle immediately), so the *IDs* still follow cross-lane
   reservation timing — canonical IDs need barrier-time reservation, an
   API-visible change left for when something actually needs it — and
   commands from *imperative* systems keep the thread-sharded path with its
   unspecified cross-shard order.

   **Sequencing**: (1) slot substrate + `Reduce` + `Extract` — done, this
   branch; mist proves both faces (`grid` → Reduce, `extract` → Extract);
   (2) `Random` + `Scratch` — done, this branch (`WaveContext` threads
   the system id + schedule tick into the prepare hooks for stream seeding);
   (3) `Collect`, then Events on the same plumbing — done, this branch
   (events readable exactly one tick after emission, deterministic order at
   any lane count); (4) `Local<T>` — done, this branch (with the
   imperative-parameter protocol mirroring `parallel_param`, so `add()` and
   `add_parallel()` now compile against the same protocol shape); (5) `Bin` and
   deterministic kernel Commands — done, this branch (canonical replay order;
   record-time ID reservation is the documented residual).
   Deliberately out: parallel sort (a utility over an Extract'd buffer),
   previous-value components (a storage feature), and any atomic/`Shared<T>`
   wrapper (the suite exists precisely so nobody needs one).

   Design notes settled in discussion (implemented for Reduce/Extract):
   - **Fold in canonical world order** — item generation order (archetype
     ascending, rows ascending), not LPT claim order: an extraction-shaped
     reduce reproduces exactly what a serial `for_each` gather
     produces, and float folds are reproducible at any lane count.
   - **Slots persist per system and are reset, not reconstructed** (`clear()`
     when the type has one, else assign `T{}`), so vector-like partials keep
     heap capacity across ticks — zero-alloc-per-tick holds. `Op` may move
     out of the partial. A `Reduce` target resource is REBUILT each run
     (reset at wave prepare), like the serial rebuild-every-tick systems it
     replaces.
   - **`Extract<T>`** avoids the reduction's double write for gather-shaped
     output: the executor knows each item's global row offset when it slices
     (prefix sums), pre-sizes the vector-like target resource once before
     dispatch, and binds each item a span over its DISJOINT slice — one write
     per element, no fold; leveling via a declared resource write. The
     `TripleBuffer::publish()` stays a one-line next-wave system.

   **Choosing between a kernel primitive and a serial system** (the rule the
   mist `grid` measurement produced). A partial+merge primitive costs
   *(per-row work / lanes) + a serial merge of everything the partials hold*,
   so ask: **how big is one item's partial at the barrier, relative to the
   rows it consumed?**
   - Partial is O(1) or O(few buckets) — sums, bounds, means, small
     histograms: the fold compresses; the kernel wins as soon as per-row work
     is nontrivial.
   - Partial is O(rows) — every row emits an entry (`Collect`, `Bin`): the
     merge re-does O(n) serially, so the kernel wins only when *producing*
     each entry costs real compute. (`Extract` escapes this: disjoint spans,
     no merge at all.)
   - The losing shape: per-row work ≈ zero AND keys uncorrelated with row
     order (mist's grid — a cheap scatter-add over many cells). Partials
     neither compress nor come cheap; keep it serial.
   The A/B is safe by construction — `add` ↔ `add_parallel` is a one-word,
   behavior-preserving switch (identical results at any lane count), so
   measure with `ScheduleReport` and believe the numbers. **[done]** The
   profiler surfaces exactly this: each system emits a `SystemWork` event
   with measured busy time (per-item timing summed across lanes) plus its
   barrier prepare/finish hook durations, so a kernel whose finish fold
   costs as much as its dispatch flags itself in `ScheduleReport`'s
   prep/fin columns, in `ScheduleTrace`'s CSV, and in the HTML report
   (`tools/schedule_report`).

   **Data layout as the missing precondition — spatial/key sorting
   (in prototype).** The grid shape loses above because work items are
   contiguous ROW ranges while its keys are spatially random, so partials
   can't compress. Periodically reordering an archetype's rows so row order
   correlates with the key (sort by cell index every N ticks; coherence
   decays slowly, the sort amortizes) fixes the precondition instead of
   retiring the tool: each item then touches a compact key range, the same
   `Reduce<Grid, Op, SparsePartial>` compresses, and neighborhood-reading
   kernels (mist's `steer`) gain cache locality on top. Pieces:
   - **[done — this branch]** `Archetype::permute_rows(perm)` /
     `IColumn::apply_permutation`: apply a permutation to every column
     (described and dynamic) plus the row→entity table, then fix up the
     entity records — bulk swap-and-pop-family bookkeeping, structural, so it
     runs outside `run()` like other maintenance.
   - **[done — this branch]** `World::sort_rows<C>(key)`: stable sort of every
     archetype containing C by a key of C's value — stable + canonical keys
     ⇒ the permutation is identical at every lane count, so the determinism
     guarantee survives sorting.
   - Measured (mist grid shape, 40k rows, 4-core container): sorting flips
     the kernel Reduce grid from a 2× LOSS to a 2× WIN in isolation
     (~250-300µs serial → 125µs kernel-over-sorted at 4 lanes), and the
     bitwise lane-invariance checks pass with sorting on.
   - **[done — this branch]** The sort's constant, cut 6×: integral keys with
     a compact range (grid cells) take a counting sort (stable, same
     canonical permutation; 5.4ms → 0.88ms per 40k rows — the comparison
     sort was 4.7ms of the 5.4), and a keys-pass early exit skips
     already-sorted archetypes (1.85ms → 0.24ms) plus a `min_disorder`
     threshold (fraction of adjacent key descents) to leave nearly-coherent
     layouts alone.
   - **[done]** Structural upkeep is a regular system, not a bespoke hook.
     Two general primitives replace the old `Schedule::add_maintenance`:
     a runtime `every` cadence on `add`/`add_parallel` (a system runs only on
     ticks where `tick % every == 0`; a wave emptied by skips runs no barrier),
     and `Commands::sort<C>(key)` — a deferred `World::sort_rows` that applies
     at the barrier (world-quiescent, single-threaded) like every other
     command. So a re-sort cadence is just a `phase{-1}` system that records a
     sort command every N ticks: it inherits the schedule's timing, reporting,
     and exception handling with no maintenance-specific path. (The sort's cost
     lands in that wave's `flush_us`; commands are tagged with their recording
     system and the barrier apply is timed **per system** (the `cmd_us` trace
     column), so the report attributes each system's own flush exactly — a
     `flush` column folded into the amortized cost plus a spike time-series —
     even when several command systems share a wave. So a command system ranks
     by its real budget rather than its ~0 busy.)
   - End-to-end (mist, grid flipped to the kernel Reduce + a 64-tick sort
     cadence): 4-lane tick 0.83 → 0.77 ms at 40k, 1.63 → 1.54 ms at 85k
     (lane-scaling 2.6-2.8× → 3.1-3.3×), at the documented cost of ~10% at
     1 lane — the kernel registration is a bet on lanes. A lanes × birds
     sweep (1-4 lanes × 10k-170k birds, 4-core container) shows the same
     shape at every scale: ~8-10% behind at 1 lane, parity at 2, ahead from
     3 lanes up (to ~10% at 4 lanes / 170k), with the crossover pinned
     between 2 and 3 lanes independent of flock size. Determinism checks
     stay bitwise-green with the periodic sort on. (Both variants
     collapse identically under oversubscription — 5 lanes on 4 cores is
     ~25× worse; the always-spin pool's rule stands: lanes ≤ cores minus
     the other hot threads.)
   - Roadmap: sort-by-entity/multi-component keys; in-place permutation if a
     workload ever makes the gather scratch matter.

   **Parallel two-pass `Bin`** — **[done, with a measured caveat]**. The merge
   is now a tiled counting sort: the entries are the concatenation of the
   per-item partials, split into tiles that count and scatter on the pool
   (`FinishItemsFn` gained a `WorkerPool&` — a barrier fold runs after the
   join, so the pool is idle and safe to dispatch on), with only the
   O(tiles × buckets) prefix left serial. Offsets are assigned bucket-major
   with tiles in order, so within-bucket order stays canonical and the result
   is bitwise identical at any lane count (`test_parallel_paths.cpp`,
   `bin_merge_is_lane_invariant_at_scale`, over 1.28M entries).

   The caveat is the interesting part: **the merge is memory-bound, so the
   tiling is not where the win came from.** Measured on 4 cores at 4 lanes,
   vs the old merge: −32% (200k entries → 64 buckets), −20% (→1000), −15%
   (→6205), −19% (→20000), −41% and −57% at 2M entries. Almost all of that is
   the *serial* rewrite — skipping the max-key scan when `reserve_buckets`
   already covers the keys, and 32-bit cursors instead of 64. Tiling itself
   only helps past ~1M entries (14% at 2M into dense buckets) and *hurt* at
   200k, so it is gated on entry count AND on bucket density: tiles split each
   bucket, so a tile's slice must be worth a cache line or the scatters
   ping-pong the same lines. Sparse buckets — including the FMM
   interaction-list shape (~31 entries per bucket) — keep the serial path.
   Expect the gate to be pessimistic on a host with more lanes and more
   memory channels; it is tuned on the only hardware available.

## 7. Tests, benchmarks, CI

- **[done]** CI (GitHub Actions): gcc + clang, Debug + Release, ASAN/UBSAN and
  TSAN legs (`ECS_SANITIZE` CMake option), warnings enabled for tests.
- **[done]** Multi-lane `for_each_chunk`/`for_each_parallel` tests (≥10k rows,
  every row written exactly once); command recording from parallel kernels;
  nested-dispatch error; throwing-flush no-replay regression tests.
- **[done]** Benchmarks: `scale_bench` no longer hardcodes an 8-lane pool on a
  4-core host; `bench::keep` uses the `"+r,m"` constraint; `gather_bench`
  unified onto `bench.hpp`.
- Roadmap: seeded model-based structural-ops test (random
  spawn/add/remove/destroy/set vs a map oracle); components with
  `std::string` fields through relocation under ASAN; throwing
  observer/missing-resource paths; tag components through the full archetype
  path; doctest (or extend `check.hpp` with variadic `CHECK`, value-printing
  `CHECK_EQ`, per-case ctest registration); `-march=native` as an option;
  optional EnTT/flecs comparison; a wasm CI leg running `web/test/smoke.cpp`
  under node. **[done]** structural churn / fragmentation / random-access
  benchmark dimensions (churn/access suites), machine-readable results
  (`ECS_BENCH_OUT`), per-suite dispersion via the shared `bench::Dist`.

### Benchmark-informed development workflow (roadmap)

The goal: never guess about the impact of a change — identify regressions
across reproducible runs and configurations, with per-system attribution.
Three cadences, matched to the measurement layers that already exist:
the microbenchmarks (inner loop), deterministic `mist-headless` +
`ScheduleTrace` (per-PR attribution), and the lanes×population sweeps
(longitudinal trends).

1. **[done]** Machine-readable results: every suite (+ `mist-headless`)
   appends `(suite,name,metric,value,unit,better,config…)` rows wrapped in a
   run-metadata envelope (timestamp/commit/build/compiler/cpu) to
   `ECS_BENCH_OUT=<csv>`; `mist-headless` gained `ECS_TRACE` so deterministic
   macro runs produce comparable schedule traces. See `benchmarks/README.md`.
2. **[done]** `schedule-report compare base.csv head.csv`: side-by-side HTML
   + console/CSV deltas per system (busy/prepare/finish), noise-aware
   significance from the per-tick distributions, `--fail-on-regression` for
   gating.
3. **[done]** Interleaved A/B driver (`benchmarks/ab.py`): builds the base
   ref in a git worktree + the working tree as head, runs the suites
   round-robin with the order flipped each round, and compares paired
   per-round samples per (suite,name,metric,entities,lanes) key — paired
   t-test at 99% (a fixed 3σ is anti-conservative at 3–7 rounds) plus a
   relative threshold (default 5%). Validated both ways: same-vs-same runs
   clean; a planted `get<C>` pessimization is caught (+200%) with nothing
   else flagged. Wall time is only trustworthy relative, same machine,
   same session.
4. CI wiring beyond the smoke leg **[smoke done]** (tiny-size run of every
   suite on the Release legs asserts rows + envelope, never timings):
   label-triggered or nightly job running the A/B driver (base and head in
   the *same job* — never compare against stored absolute numbers from
   other runners), PR comment with the delta table, HTML reports as
   artifacts. Allocations/tick gates hard (noise-free). Later:
   instructions-retired counters (perf/cachegrind) for de-noised per-PR
   gating on shared runners.
5. Longitudinal dashboard: nightly full suite + sweeps on one consistent
   machine appended to a data branch; trend page over the accumulated
   results CSV (the envelope columns make rows self-describing).

Pool-width matrix **[done]**: the scaling suites (scale/primitives/schedule)
sweep the lane count via `bench::lane_set()` (`ECS_LANES`, default the
power-of-two ladder to hw) instead of timing one oversubscribing hw point;
the results CSV's `lanes` column makes each run a lanes×metric matrix, and
`schedule-report bench` renders speedup-vs-lanes curves against an ideal
diagonal so the turnover past the physical-core count is a picture.

## 8. Wasm & JS interop

- **[rejected]** Idle-lane parking under Emscripten (see §2) — the Worker
  100%-burn problem needs an explicit app-driven idle API instead; roadmap.
- **[done]** `-msimd128` (+`-mrelaxed-simd`) on web demo targets — the SoA
  vectorization pitch was scalarized on the web.
- **[done]** `-fwasm-exceptions` on the embind targets — a JS typo in a field
  type string aborted the whole runtime instead of throwing catchably.
- **[done]** `defineParallelSystem` built `Float32Array` views for every field
  regardless of declared type — `i32`/`f64`/`u32` were silently reinterpreted
  as f32 in parallel kernels; views now honor `FieldType`.
- Roadmap: document the heap-growth typed-array detachment hazard in the
  `defineSystem` contract (structural ops mid-kernel on the
  `ALLOW_MEMORY_GROWTH` build); growable SharedArrayBuffer to drop the MT
  128 MB pin; auto-fallback from the MT page to the ST module when
  `!crossOriginIsolated`; `coi-serviceworker` note for header-less hosts;
  comment the wasm32 pointer-truncation assumptions (memory64 not worth
  adopting yet); a `PROXY_TO_PTHREAD` + OffscreenCanvas demo; a hand-written
  `.d.ts` for `DynamicWorld`; per-archetype view caching keyed by an
  archetype-generation counter.

## 9. Examples & tooling (roadmap)

- `examples/hello.cpp` minimal example built in CI (the README snippet is
  untested documentation); lead the quickstart with an `add_parallel` system
  (parallelism lives on the executor, not the query).
- De-macOS-ify `particles`/`mist` (glad instead of CGL includes) or point
  non-Mac users at the wasm build explicitly.
- Best missing demo: fixed timestep + render interpolation (publish
  prev+curr, lerp by accumulator remainder) — completes the snapshot-handoff
  story with no new library features. Also: app-level events, native dynamic
  components, `Schedule::remove`.
- Tools: Chrome-trace/Perfetto export (keep begin timestamps in
  `ScheduleTrace`; ~80 lines buys the full timeline UI); per-lane
  instrumentation in `WorkerPool` (lane imbalance and join-spin cost are
  invisible); cost-colored `schedule_viz` nodes from `ScheduleReport`;
  advertise the mist metrics/filmstrip harness in the top README.
- Backport the web port's overrun resync into the native sim loops;
  deduplicate the render-thread body and the three `gl_util` copies into
  `examples/common/`; drop the hardcoded `Entity{0,0}` in `nbody.cpp`.
