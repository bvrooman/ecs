# Adaptive FMM on this ECS — a feasibility assessment

**Question.** Can this library carry an *adaptive fast multipole method* n-body
simulation?

**Verdict: yes.** `examples/fmm2d.cpp` is a working one — adaptive quadtree,
dual-tree traversal, verified against a direct O(N²) sum — and every FMM pass in
it is an ordinary system whose access the scheduler derives and orders by
itself. Getting there took six library changes, because the first version of
the spike could *only* be written by stepping outside the access model. Those
changes are the real output of this exercise; §3 is the list, with the
before/after each one bought.

The tree build was the last serial stage, and four fifths of it is the
interaction-list traversal. That turned out not to be inherently serial either:
§6.1 measures it, shows why the obvious parallelization is *wrong*, and gets the
correct one onto the lanes twice — first through `Exec` (§3.5), an escape hatch
that fans an arbitrary index space from inside a serial system, and then through
`partition_by<K>` (§3.6), which says the same thing declaratively: rows sharing a
key are ONE work item. The second is the one that belongs in an ECS. It is
faster, and more to the point it puts the traversal back inside the executor —
leveled, overlapped and load-balanced like everything else — instead of beside
it.

```
$ ./build/examples/fmm2d                  # n leaf_cap ticks max_lanes
fmm2d: n=50000 leaf_cap=32 p=12 buffer=2.00 max_level=8
schedule: 25 systems, 22 waves
lanes=1  305.39 ms/tick   nodes=4505 leaves=3379 depth=8 m2l=102782 p2p=35441
lanes=4   76.91 ms/tick   ...
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
| tree build | recursive, sequential | `add_serial` writing a `Tree` resource |
| interaction lists | one independent walk per subtree | `add_parallel` + `partition_by<Subtree>`, writing `M2LList`/`P2PList` |
| P2M | leaf ← its contiguous particle rows | `Query<Mp, NodeRef const>` + `ColumnView<Pos, Charge>` |
| M2M | level L ← level L+1 | `Query<Mp, …, Lvl<L> const>` + `ColumnView<Mp, NodeRef, Lvl<L+1>>` |
| M2L | node ← many nodes, any level | ONE `Query<Lc, NodeRef const>` + `ColumnView<Mp, NodeRef>` |
| L2L | level L ← its parents at L−1 | `Query<Lc, …, Lvl<L> const>` + `ColumnView<Lc, NodeRef, Lvl<L-1>>` |
| L2P + P2P | per particle, gathers other particles | `Query<Field, …>` + `ColumnView<Pos, Charge>` + `ColumnView<Lc, NodeRef>` |

One entity per tree node, in the archetype of its level, carrying `Mp`, `Lc` and
its interaction lists as components. Two details make it work:

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

The executor sizes items with a floor of `kMinItemRows` (1024), relaxed so a
large system lands near `kTargetItemsPerSystem` items. That is a policy for
*cheap per-row work*, and an FMM kernel breaks it in **both** directions:

- **Too coarse below the floor.** A tree-level pass is a few hundred rows of
  thousand-flop work, so the floor made it ONE item — one lane, at any lane
  count.
- **Too coarse above the target.** The near field has rows to spare, but per-row
  cost tracks local density, so the 64-item cap left items differing ~2× and the
  heaviest became the straggler every lane waits on.

`grain{n}` is therefore *exactly n rows per item*, replacing the sizing rather
than raising its floor — an earlier floor-only version could fix the first
problem but not the second.

| pass | items before | items after | effect |
|---|---|---|---|
| `m2m.*` | 1–3 | 12–20 | 0.94 → **0.44 ms** at 4 lanes |
| `l2l.*` | 1–3 | 16–45 | 1.41 → **0.55 ms** at 4 lanes |
| `m2l` | 7 | 101 | 15.55 → **11.95 ms**; scaling 2.95× → **3.97×** |
| `evaluate` | 65 | 391 | 4-lane wall unchanged; longest item 15.03 → **3.07 ms** |

The `evaluate` row is the one that only the exact-size semantics can express,
and it buys nothing on 4 lanes — it is worth 33× → 46× at 64 (§5.1). More items
cost dispatch bookkeeping and re-bind the system's parameters once per item, so
this is a tuning knob, not a default to lower globally.

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

### 3.5 `Exec` — fan out work that has no rows

Every route to the pool was row-shaped: a parallel system is sliced by the rows
its Query matches. An irregular stage with parallelism but no rows — a tree
build, a traversal, a broad-phase partition — therefore sat in an `add_serial`
system at one item on one lane, however many lanes existed.

`Exec` fans an arbitrary index space onto the schedule's own lanes. The
mechanism already existed; what was missing was the guarantee and a handle. A
dispatch is not re-entrant, so a system running as one item of a fanned wave
must not dispatch — but `Exec` declares its system **exclusive**, so the system
is alone in its wave, the wave holds exactly one item, and a one-item wave is
run inline on the calling thread (`for_each_index` returns before touching the
job slot). The pool is idle at that moment, so the body's dispatch is a fresh
one. The cost is the exclusivity: such a system overlaps with nothing else in
its phase.

Measured on the tree build in §6.1: −36% and −46% at 4 lanes, bit-identical
output.

### 3.6 `partition_by<K>{}` — when the unit of work is a GROUP of rows

`Exec` works, and it is the wrong shape. It is an escape hatch: the body reaches
around the executor to the pool, so it must know the lane count to size its own
index space, it declares itself exclusive (overlapping with nothing else in its
phase), and the work it fans out is invisible to the scheduler — not leveled,
not load-balanced against other systems, not weighted in the wave's LPT sort.
Every one of those is a property the executor already provides for row-shaped
work. The traversal did not need a different executor; it needed a different
description of what one work item *is*.

The default sizing may cut a parallel system's matched rows anywhere, which is
exactly right when rows are independent and exactly wrong when the unit of work
is a *set* of them. `partition_by<K>{}` declares the equivalence classes: rows
sharing a value of key component `K` become **one** work item.

```cpp
sched.add_parallel("traverse", fn, partition_by<Subtree> {});
```

The executor scans `K` down each matched archetype for runs of equal value,
gathers the runs of one key — they may span archetypes — into one item, and
emits items in ascending key order. Three consequences matter here:

* **A group is never split across lanes**, which is what makes the body's writes
  safe: the item's rows *are* the boxes its walk will write.
* **The item lands in the same flat list as every other system's**, so it is
  leveled, dynamically claimed, and overlapped. The FMM traversal now shares a
  wave with the deepest M2M level instead of owning one.
* **A group's row count is its cost**, which is a far better weight than uniform
  slicing gives, so the LPT sort can actually order the wave.

It also deletes a question the `Exec` version had to answer: how many pieces to
split into. `Exec` sizes an index space, so it asks for `exec.lanes()`.
`partition_by` declares groups and lets the executor match them to hardware, so
the cut level became a property of the tree alone.

The cost is a linear boundary scan of one contiguous field buffer per archetype
at wave-build time, and a precondition: `K` must be a single integral field, and
rows should be sorted by it or a group fragments into many small runs (still one
item — grouping is by value — but a scattered one). §6.1 has the measurement.

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
| `build` (serial head only) | 1 | 0.73 ms | — | 0.66 ms | 1.11× |
| `traverse` (`partition_by`) | 65 | 3.43 ms | — | 0.68 ms* | — |
| `p2m` + `assign-leaf` (one wave) | 70, 65 | 4.52 ms | 2.43 ms | 1.19 ms | 3.79× |
| `m2m.*` (8 waves) | 12–20 | 1.23 ms | 0.75 ms | 0.44 ms | 2.77× |
| `m2l` | 70 | 47.40 ms | 24.61 ms | 11.95 ms | 3.97× |
| `l2l.*` (8 waves) | 16–45 | 1.78 ms | 0.94 ms | 0.55 ms | 3.21× |
| `evaluate` | 65 | 576.2 ms | 290.0 ms | 148.5 ms | 3.88× |
| **whole tick** | | **644 ms** | **326 ms** | **165 ms** | **3.89×** |

\* `traverse` shares its wave with `m2m.7`, so its 4-lane figure is that wave's
wall minus what `m2m.7` alone costs. Overlapping with the rest of the wave is the
point of §3.6 — the number has no standalone meaning, which is exactly the
difference from an exclusive `Exec` system.

Against the pre-change version of the same spike (identical physics, verified by
identical output): whole tick 173 → 165 ms at 4 lanes, and whole-tick scaling
3.66× → **3.89×** of a 4-core machine. In the tree-heavy regime (`leaf_cap=4`:
23.5k nodes, 485k M2L pairs) the tick goes 178 → 166 ms.

Results are lane-invariant: the same run at 1 and 4 lanes reports an identical
force error (8.780e-07), as the library's determinism guarantee requires.

Absolute performance is not the point of the spike — the kernels are unoptimized
`std::complex<double>` with a `log()` per direct pair, and the near field is
computed with the potential a force-only sim would skip. The *shape* is.

### 5.1 Beyond four lanes

Only a 4-core machine was available, so higher lane counts are answered by
**re-scheduling the measured work items offline**. The executor claims items
from one list per wave, so a wave's makespan on N lanes is a greedy
longest-first assignment of its item durations — the same policy the pool
implements. Item durations come from a run at 1 lane (an instrumented build
dumping every item's measured busy time), and each wave is charged the ~2 µs
barrier measured on this box.

The model reproduces the one point that can be checked: it predicts 160.6 ms at
4 lanes against 166.1 ms measured, 3% optimistic. It is an **upper bound** —
1-lane item durations carry no memory-system contention, which N lanes would.

| lanes | predicted tick | speedup | vs ideal |
|---|---|---|---|
| 1 | 633.3 ms | 1.00× | 1.00× |
| 2 | 318.2 ms | 1.99× | 1.00× |
| 4 | 160.6 ms | 3.94× | 1.01× |
| 8 | 81.7 ms | 7.75× | 1.03× |
| 16 | 42.7 ms | 14.83× | 1.08× |
| 32 | 23.2 ms | 27.25× | 1.17× |
| 64 | 13.7 ms | 46.29× | 1.38× |

The item-count ceilings that motivated `grain{n}` are gone: no system is now
capped below ~34 lanes, where before `m2m`/`l2l` were capped at 1–3 and
`evaluate`'s longest item capped it at 38.

| system | items | work | longest item | most lanes it can use |
|---|---|---|---|---|
| `build` | 1 | 2.33 ms | 2.33 ms | **1** |
| `m2m.*` | 56 | 1.39 ms | 0.10 ms | 14 |
| `assign-leaf` | 65 | 1.64 ms | 0.10 ms | 17 |
| `p2m` | 101 | 2.92 ms | 0.08 ms | 34 |
| `l2l.*` | 100 | 1.76 ms | 0.04 ms | 48 |
| `m2l` | 101 | 47.6 ms | 0.80 ms | 59 |
| `evaluate` | 391 | 575.7 ms | 3.07 ms | 188 |

What binds at 64 lanes is §6.1: the serial tree build.

**This simulation predates the `partition_by` rewrite and has not been re-run** —
it needs the instrumented per-item build, and the numbers above are the ones that
motivated the change, so they are left as they were. What it would now see is the
`build` row splitting in two: `build` keeps 1 item but only **0.73 ms** of work
(the BFS subdivision and the serial head of the descent), and the traversal
becomes `traverse` at **65 items / 3.43 ms**. The row that reads "most lanes it
can use: **1**" is what `partition_by` was for, and it is the row that moved.

## 6. What remains

### 6.1 The tree build: was serial, now mostly isn't

A recursive build has no row shape, so it was an `add_serial` system: one opaque
work item, one lane, whatever the lane count.

Inside it, the two halves are not alike:

| | key read | subdivide (BFS) | **interaction-list traversal** |
|---|---|---|---|
| leaf_cap=32 | 0 (in place) | 445 µs | **1.80 ms (79%)** |
| leaf_cap=4 | 0 | 1.46 ms | **8.4 ms (84%)** |

The BFS subdivision is genuinely sequential — level L+1 does not exist until
level L has been split. The traversal is not, and it is the 80%.

**The obvious parallelization is wrong.** Descending from the root once per
*target* node is perfectly row-shaped, and it does not reproduce the same
lists: measured force error 0.64 instead of 9.6e-07. A dual-tree traversal's
decisions are path-dependent — when it splits the target at (A, B), the pair
moves to A's children with B *already descended*, and a fresh descent for a
child re-derives a different stopping point.

**What works is splitting the same recursion.** Descend serially until every
target reaches a cut level, recording where each stopped; then finish each cut
node's subtree independently. Subtrees of distinct cut nodes write distinct
nodes' lists, so there is no merge at all — and because the seeds are recorded
in the order the serial walk would reach them, each node's list comes out in
the same order. The demo's output is bit-identical either way (9.595e-07).

That leaves one independent task per cut box, and there are two ways to say so.
Both were built; the difference between them is the point of §3.6.

**Attempt 1 — fan out by hand (`Exec`, §3.5).** Keep the traversal inside the
serial `build` system and dispatch the subtrees onto the schedule's lanes from
inside it. It works, and it costs three things: the system is *exclusive*, so it
overlaps with nothing else in its phase; the body sizes its own index space, so
it asks the pool how many lanes exist — hardware in application code; and the
lists stay in the `Tree` resource, so a serial pass afterwards has to flatten
`vector<vector<uint32_t>>` into CSR.

**Attempt 2 — declare the partition (`partition_by<Subtree>`, §3.6).** Mark each
box with the cut-level ancestor it descends from, move the interaction lists onto
the box (`M2LList`/`P2PList` components), and register the traversal as an
ordinary `add_parallel` system partitioned by that mark. One work item is now one
subtree, and the rows of that item are exactly the boxes its walk will write — so
the traversal writes its own rows, like every other system in the file. The
addressing is O(1) and needs nothing unsafe: within a level, row == slot, and a
subtree's boxes at a level are contiguous (level L is generated parent by parent,
so it comes out ordered by cut ancestor), so `chunk::row_begin()` turns a box id
into a chunk index.

100k particles, 4 lanes, `ScheduleReport` wave wall times. "build span" is
waves 2–4 end to end — everything from the tree build to the point where the
interaction lists exist — so the two are compared on the same interval even
though the work sits in different waves:

| 4 lanes | `build` wave (serial, 1 item) | build span (waves 2–4) |
|---|---|---|
| leaf_cap=32, `Exec` | 1.68 ms | 3.55 ms |
| leaf_cap=32, `partition_by` | **0.66 ms** (−61%) | **3.24 ms** (−9%) |
| leaf_cap=4, `Exec` | 5.62 ms | 8.30 ms |
| leaf_cap=4, `partition_by` | **1.81 ms** (−68%) | **7.30 ms** (−12%) |

Output is bit-identical throughout (9.595e-07 at leaf_cap=32, 9.628e-07 at
leaf_cap=4, and the same M2L/P2P pair counts).

The span number understates it and the `build` number is the one to read. What
shrank by ~3× is the part that is *structurally serial*: one system, alone in its
wave, one work item. The traversal did not get faster — it moved. It is now 65
items in a shared wave, overlapping the deepest M2M level, claimed out of the
same list as everything else. And the serial CSR flatten is gone: the lists are
written where they are read.

Total work is a wash — at 1 lane, where nothing overlaps, `leaf_cap=4` measures
13.05 ms of build against 12.88 ms (`build` + `mark-subtrees` + `traverse`),
inside run-to-run variance. The flatten it removes is paid back by seeding each
box's list from the serial head and by 65 item binds instead of one. That is the
right trade and worth stating plainly: `partition_by` did not make the traversal
cheaper, it made it *schedulable*.

§5.1's simulation is the reason this matters more than 12%: at 16 lanes the
`leaf_cap=4` tick was 49% serial build. Cutting the serial part from 5.6 ms to
1.8 ms is what moves that ceiling; a cadence (`every{n}`) remains the answer for
the BFS subdivision that is left, and is standard for slowly-evolving
distributions.

**A road not taken.** A traversal emits a variable-length list per target, which
looks exactly like `Bin<V>` — bucket = target node, value = source node,
counting-sorted at the barrier into a CSR. Prototyped, and the barrier merge
measured **3.20 ms** at ~4.6× the true pair count. Worse, FMM's lists are ~31
entries per bucket, which is too sparse for the tiled merge to help (see the
`Bin` work). The partition avoids the merge entirely — a group's rows are its
own, so there is nothing to reconcile — which is why it wins.

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
3. **The tree's geometry and topology are a resource**, read via `Res<Tree>` —
   flat parallel arrays have no component shape. The **interaction lists are
   not**: there is one per box, so they are box components, which is what lets
   the traversal write them.
4. **The traversal is `add_parallel` + `partition_by<Subtree>`**, after a serial
   head that descends to a cut level. One item per subtree, and the item's rows
   are the boxes it writes.
5. **Every gather is a `ColumnView`**, narrowed with a tag wherever the source
   level is statically known.
6. **Give every node pass `grain{n}`** — their row counts are far below the
   default floor.
7. **Register in dependency order** (deepest-first upward, shallowest-first
   downward) and let conflicts place the barriers; use `phase{}` only where the
   dependency is structural.
8. **Rebuild the tree on a cadence** (`every{n}`), re-sorting rows every tick.

## 8. What this does not answer

- **3D.** Bigger kernels (spherical harmonics, ~189-box V-lists) and higher
  arithmetic intensity per node, which makes `grain{n}` matter more, not less.
  The mapping is unchanged.
- **Beyond 4 cores — measured indirectly.** §5.1 re-schedules the real item
  durations rather than guessing, and is calibrated against the one lane count
  that could be checked, but it models neither memory-bandwidth contention nor
  NUMA. Treat it as the ceiling the *item structure* allows, and re-measure on
  real hardware.
- **Distributed or GPU FMM.** Out of scope for a single-process shared-memory
  executor.
- **The P2996 reflection backend** (needs GCC 16) — built on the portable
  backend only.
- **A full time-stepping simulation.** The spike computes forces; integration and
  the render handoff are already covered by the other examples.
