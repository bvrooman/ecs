# Adaptive FMM on this ECS — a feasibility assessment

**Question.** Can this library carry an *adaptive fast multipole method* n-body
simulation?

**Verdict: yes.** `examples/fmm2d.cpp` is a working one — adaptive quadtree,
dual-tree traversal, verified against a direct O(N²) sum — and every FMM pass in
it is an ordinary system whose access the scheduler derives and orders by
itself. Getting there took four library changes, because the first version of
the spike could *only* be written by stepping outside the access model. Those
changes are the real output of this exercise; §3 is the list, with the
before/after each one bought.

What remains genuinely unsupported is one thing: the tree build and its
traversal are recursive, so they have no row-shaped parallel form and run
serially (§6.1). That is the scaling limit past roughly 16 lanes, and it is
inherent to the executor's model rather than a gap to be patched.

```
$ ./build/examples/fmm2d                  # n leaf_cap ticks max_lanes
fmm2d: n=50000 leaf_cap=32 p=12 buffer=2.00 max_level=8
schedule: 23 systems, 22 waves
lanes=1  236.45 ms/tick   nodes=4505 leaves=3379 depth=8 m2l=102782 p2p=35441
lanes=4   63.94 ms/tick   ...
accuracy vs direct (200 samples): force L2 rel err = 1.198e-06, potential L2 rel err = 4.820e-08
```

The spike is the 2D log-kernel FMM (Greengard–Rokhlin): P2M/M2M/M2L/L2L/L2P plus
a direct near field, an adaptive quadtree (subdivide while a box holds more than
`leaf_cap` particles), and a dual-tree traversal for the interaction lists — so
source and target boxes sit at *different* levels, which is the adaptive case
rather than a uniform grid. 3D changes the kernels and the list sizes, not one
thing about the ECS mapping.

Measurements: 4-core Intel Xeon @ 2.10GHz, clang-18, Release (`-O3 -DNDEBUG`),
portable reflection backend, 100k particles unless stated. Run-to-run spread on
this shared box is roughly ±10%, so every timing is a median of three runs and
no claim rests on a smaller difference. Work-item counts are exact — they are
structural, not timed.

---

## 1. The one property everything follows from

**A `Query` is bound to one work item, so it shows a system only that item's row
slice** — which is exactly what makes a parallel write race-free by
construction. FMM's kernels are *gathers*: M2M reads its children, M2L reads
~25 source nodes anywhere in the tree, P2P reads neighbouring particles. Every
one of those reads rows outside the slice.

The shape needed for that already existed twice over — `Query::for_each_chunk`
clipped to the item, and `WorldView::for_each_chunk` over whole columns:

```
5000 rows, one archetype, inside one parallel body:
  item: query chunk = 1024 rows starting at column offset    0   |   view chunk = 5000 rows
  item: query chunk = 1024 rows starting at column offset 1024   |   view chunk = 5000 rows
  ...
```

What was missing was a whole-column read with an *honest access declaration*.
`WorldView` declares "reads everything", which serializes the system against
every writer in its phase, so a schedule built out of gathers collapses into a
chain of one-system waves. The first version of this spike therefore put the
expansion coefficients in a resource and wrote them through a raw pointer —
outside the model entirely — and hand-placed 24 `phase{}` numbers to order
passes the scheduler could no longer see.

## 2. Consequence: nodes are entities that carry their expansions

With the changes in §3, the mapping is direct:

| FMM stage | its shape | ECS construct |
|---|---|---|
| particle storage | SoA over N | components in one archetype |
| Morton key | per-row map | `add_parallel`, `Query<Pos const, Key>` |
| spatial sort | permute rows by key | `Commands::sort<Key>` (`World::sort_rows`) |
| tree build + lists | recursive, irregular | `add_serial` writing a `Tree` resource |
| P2M | leaf ← its contiguous particle rows | `Query<Mp, NodeRef const>` + `ColumnView<Pos, Charge>` |
| M2M | level L ← level L+1 | `Query<Mp, …, Lvl<L> const>` + `ColumnView<Mp, NodeRef, Lvl<L+1>>` |
| M2L | node ← many nodes, any level | ONE `Query<Lc, NodeRef const>` + `ColumnView<Mp, NodeRef>` |
| L2L | level L ← its parents at L−1 | `Query<Lc, …, Lvl<L> const>` + `ColumnView<Lc, NodeRef, Lvl<L-1>>` |
| L2P + P2P | per particle, gathers other particles | `Query<Field, …>` + `ColumnView<Pos, Charge>` + `ColumnView<Lc, NodeRef>` |

One entity per tree node, in the archetype of its level, carrying `Mp` and `Lc`
as components. Two details make it work:

- **Writes stay own-row.** Every pass writes its own rows' expansions through
  its Query, so the disjointness that makes parallel items safe still holds.
  L2L is expressed as a *pull* from the parent rather than a push to the
  children, because a system may only write rows it owns.
- **Reads are narrowed by tag.** A `ColumnView` matches every archetype
  *containing* its components, so naming `Lvl<L+1>` restricts an M2M read to
  exactly the level below — provably disjoint from the level it writes, rather
  than disjoint by the author's argument.

Node entities are pooled and never destroyed, so a steady-state tick records no
commands and allocates nothing.

## 3. What building it changed in the library

### 3.1 `ColumnView<Cs...>` — read every row, declare exactly what you read

`include/ecs/world/column_view.hpp`. Read-only, non-sliced, one chunk per
matching archetype with full columns — the same access `WorldView` already
allowed, declaring reads on exactly `Cs...` instead of on everything.

It is not a new data path (it forwards to `World::for_each_chunk`); the value is
entirely in the conflict analysis. Measured, on the spike's schedule:

| | before (resource + raw pointer) | after (`ColumnView`) |
|---|---|---|
| hand-placed `phase{}` numbers | 24 | **3** |
| systems / waves | 25 / 25 | 23 / 22 |
| writes outside the access model | all coefficient writes | none |

The three remaining phases order the pipeline head (`morton` → `sort` →
`build`), where the dependency is a *structural* effect — a row sort recorded as
a command — which access analysis does not model (§6.2). Everything downstream
now orders itself, and the scheduler finds parallelism the hand-phased version
could not express: `p2m` and `assign-leaf` are independent, and it puts them in
the same wave.

### 3.2 `grain{n}` — work-item granularity per system

`WavePlan::kMinItemRows` is 1024, which assumes cheap per-row work. A tree-level
pass is the opposite — thousands of flops per row, and few rows — so the default
pinned each level to **one item, and therefore one lane, at any lane count**.
`grain{n}` overrides the floor per system; the ~64-item target still caps the
count, so it can only raise a small system's item count, never shatter a large
one.

| pass | items before | items after | 4-lane wall before → after |
|---|---|---|---|
| `m2m.*` | 1–3 | 12–20 | 0.94 → **0.44 ms** |
| `l2l.*` | 1–3 | 16–45 | 1.41 → **0.55 ms** |
| `m2l` | 7 | 70 | 15.55 → **11.95 ms** |

`m2l` is the one that matters: it went from 2.95× to **3.97×** on 4 lanes. Seven
lumpy items (row-range slices of a node pool whose per-row cost varies by orders
of magnitude across levels) became 70 homogeneous ones.

### 3.3 `chunk::row_begin()` — where a slice sits

The executor hands a body rows, not their positions, so a body could not tell
where its slice sat in the archetype. FMM needs that constantly: to skip its own
row in the direct sum, and to join rows against anything indexed the same way.
The spike previously recovered it with pointer arithmetic on entity spans
(`ents.data() - all_ents.data()`) — sound, but not something a user should have
to derive. `chunk::row_begin()` is the three-line accessor for the `begin_` the
chunk already held.

### 3.4 The ad-hoc read path: a per-call allocation and a global lock

Found by profiling the spike, not by reading it. `World::for_each_chunk<Cs...>`
— what `WorldView` and now `ColumnView` forward to — built a `Signature` (a
`std::vector`, so a heap allocation) and took the query-cache mutex **on every
call**. A gather-shaped body calls it once per work item, so this was squarely
on the hot path.

The `Signature` is now a per-instantiation static (the same idiom
`Query::required()` already used) and the resolved match list is memoized per
(world instance, archetype generation) in a thread_local, so a steady-state call
is two relaxed atomic loads:

| | before | after |
|---|---|---|
| one `for_each_chunk<Pos>` call | 26 ns | **3 ns** |

This one is a straight win for existing `WorldView` users too.

## 4. What the library already gave for free

1. **The spatial sort is first-class.** `World::sort_rows<C>(key)` counting-sorts
   rows by a compact integral key and patches every entity record;
   `Commands::sort` defers it to a barrier. FMM's central layout precondition —
   *a box's particles are a contiguous row range* — is one line, and the
   permutation is identical at any lane count.
2. **Dispatch is cheap enough that 22 barriers per tick do not matter**: ~1–3 µs
   per wave, so well under 0.1 ms of a 165 ms tick.
3. **Dynamic item claiming absorbs irregular work.** `evaluate` (L2P + P2P,
   per-row cost varying by an order of magnitude) hits 3.88× on 4 lanes.
4. **Zero-allocation steady state**, with `prewarm()` pre-paying the first tick.
5. **Per-system attribution is built in.** `ScheduleReport` produced every number
   here, including the work-item counts that made §3.2 visible at all.
6. **Reflection handles the coefficient types.** A component holding
   `std::array<std::complex<double>, 13>` is one column and round-trips; a raw C
   array is rejected with a named diagnostic telling you to wrap it in
   `std::array`.

## 5. Measured scaling

100k particles, leaf_cap=32, 4 cores. Each figure is the wave's wall time as the
scheduler measured it, median of three runs.

| system | items | 1 lane | 2 lanes | 4 lanes | 4-lane speedup |
|---|---|---|---|---|---|
| `morton` | 65 | 0.86 ms | 0.46 ms | 0.23 ms | 3.69× |
| `sort` (command flush) | 1 | 0.27 ms | 0.24 ms | 0.27 ms | 1.00× |
| `build` (serial) | 1 | 2.54 ms | 2.40 ms | 2.47 ms | 1.03× |
| `p2m` + `assign-leaf` (one wave) | 70, 65 | 4.52 ms | 2.43 ms | 1.19 ms | 3.79× |
| `m2m.*` (8 waves) | 12–20 | 1.23 ms | 0.75 ms | 0.44 ms | 2.77× |
| `m2l` | 70 | 47.40 ms | 24.61 ms | 11.95 ms | 3.97× |
| `l2l.*` (8 waves) | 16–45 | 1.78 ms | 0.94 ms | 0.55 ms | 3.21× |
| `evaluate` | 65 | 576.2 ms | 290.0 ms | 148.5 ms | 3.88× |
| **whole tick** | | **644 ms** | **326 ms** | **165 ms** | **3.89×** |

Against the pre-change version of the same spike (identical physics, verified by
identical output): whole tick 173 → 165 ms at 4 lanes, and whole-tick scaling
3.66× → **3.89×** of a 4-core machine. In the tree-heavy regime (`leaf_cap=4`:
23.5k nodes, 485k M2L pairs) the tick goes 178 → 166 ms.

Results are lane-invariant: the same run at 1 and 4 lanes reports an identical
force error (8.780e-07), as the library's determinism guarantee requires.

Absolute performance is not the point of the spike — the kernels are unoptimized
`std::complex<double>` with a `log()` per direct pair, and the near field is
computed with the potential a force-only sim would skip. The *shape* is.

## 6. What remains

### 6.1 The tree build and traversal cannot be parallelized — **inherent**

A recursive build/traversal has no row shape, so it is an `add_serial` system:
one opaque work item, one lane. The pool rejects nested dispatch by design, so a
serial system cannot fan out internally either.

| leaf_cap | nodes | `build`, 1 lane / 4 lanes | share of the 4-lane tick |
|---|---|---|---|
| 32 | 6205 | 2.54 / 2.47 ms | 1.5% |
| 4 | 23531 | 10.9 / 10.9 ms | 6.5% |

At 4 lanes this is nearly free; it is the **scaling** limit. Holding the measured
serial part fixed and dividing the rest (`leaf_cap=4`: 166 ms at 4 lanes, ~11 ms
of it serial), the tick would be ~50 ms at 16 lanes (78% of ideal) and ~21 ms at
64 (48%). Past roughly 16 lanes you are timing the tree build. *That is
arithmetic on measured serial fractions, not a measurement.*

The practical answer is a cadence — rebuild the tree every k ticks (`every{n}`)
and re-sort rows in between, which is standard for slowly-evolving
distributions. The structural answer would be making the traversal's *pairs*
rows, which the executor can express but only at the cost of hundreds of
thousands of spawns per tick.

### 6.2 Structural effects are outside the access model — **minor**

`sort` records a command; `build` depends on it having been applied. That
ordering is not a read/write relation, so it needs `phase{}`. Three phases for
the pipeline head is a fair price, but worth knowing: a structural dependency
will not order itself the way a data dependency now does.

### 6.3 No `Without<>` filter — **minor**

Matching is "archetype includes all requested components", so per-level
selection is done with `Lvl<L>` tags and per-level systems registered over a
compile-time `kMaxLevel` bound. This turned out to be as much a feature as a
limitation — the same tags are what let a `ColumnView` name the level below —
but the max depth is baked in at compile time, and every level costs two
registrations whether or not the runtime tree reaches it. `Without<>` remains on
the roadmap in `IMPROVEMENTS.md` §6.1.

### 6.4 Cross-row reads are disjoint by construction, not by proof — **inherent**

The scheduler knows a system reads `Mp` and writes `Mp`; it cannot know the rows
are in different archetypes. Narrowing a `ColumnView` with a tag makes the
disjointness structural and obvious at the call site, which is the best the
model can do without archetype-level access tracking.

## 7. If you build this for real

The spike's architecture, which is what the library now supports directly:

1. **Particles are entities**, one archetype, sorted by Morton key with
   `Commands::sort` every tick.
2. **Tree nodes are entities**, one per node, in their level's archetype
   (`Lvl<L>` tag), carrying `Mp`/`Lc` as components. Pools grow, never shrink.
3. **The tree topology and interaction lists are a resource**, read via
   `Res<Tree>` — variable-length CSR data has no component shape.
4. **Every gather is a `ColumnView`**, narrowed with a tag wherever the source
   level is statically known.
5. **Give every node pass `grain{n}`** — their row counts are far below the
   default floor.
6. **Register in dependency order** (deepest-first upward, shallowest-first
   downward) and let conflicts place the barriers; use `phase{}` only where the
   dependency is structural.
7. **Rebuild the tree on a cadence** (`every{n}`), re-sorting rows every tick.

## 8. What this does not answer

- **3D.** Bigger kernels (spherical harmonics, ~189-box V-lists) and higher
  arithmetic intensity per node, which makes `grain{n}` matter more, not less.
  The mapping is unchanged.
- **Beyond 4 cores.** Every lane claim past 4 is Amdahl arithmetic on measured
  serial fractions. The item counts in §5 are what to watch on a bigger host.
- **Distributed or GPU FMM.** Out of scope for a single-process shared-memory
  executor.
- **The P2996 reflection backend** (needs GCC 16) — built on the portable
  backend only.
- **A full time-stepping simulation.** The spike computes forces; integration and
  the render handoff are already covered by the other examples.
