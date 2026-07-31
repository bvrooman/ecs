# Adaptive FMM on this ECS — a feasibility assessment

**Question.** Can this library carry an *adaptive fast multipole method* n-body
simulation?

**Verdict: yes, with a caveat that decides the whole design.** The library
carries the particle half of FMM natively — SoA columns, a spatial row sort, and
data-parallel passes are exactly what it is built for. It does **not** natively
carry the tree half: FMM's tree passes are node-to-node *gathers*, and a
`Query` shows a system only its own row slice, so the tree and the expansion
coefficients end up in resources with entities acting purely as *rows to
slice*. Everything then works — but three of the library's headline properties
(derived access, automatic ordering, the parallel-primitive suite) stop
applying to the tree passes, and the tree build stays serial.

This is not a paper exercise. `examples/fmm2d.cpp` is a working adaptive FMM
built on the library, verified against a direct O(N²) sum, and every number
below is measured — the per-system figures come from the library's own
`ecs::diag::ScheduleReport`.

```
$ ./build/examples/fmm2d                  # n leaf_cap ticks max_lanes
fmm2d: n=50000 leaf_cap=32 p=12 buffer=2.00 max_level=8
schedule: 25 systems, 25 waves
lanes=1  234.28 ms/tick   nodes=4505 leaves=3379 depth=8 m2l=102782 p2p=35441
lanes=2  120.63 ms/tick   ...
lanes=4   65.39 ms/tick   ...
accuracy vs direct (200 samples): force L2 rel err = 1.198e-06, potential L2 rel err = 4.820e-08
```

The spike is the 2D log-kernel FMM (Greengard–Rokhlin): P2M/M2M/M2L/L2L/L2P plus
a direct near field, an adaptive quadtree (subdivide while a box holds more than
`leaf_cap` particles), and a dual-tree traversal for the interaction lists — so
source and target boxes sit at *different* levels, which is the adaptive case
rather than a uniform grid. 3D changes the kernels and the list sizes, not one
thing about the ECS mapping below.

Measurements: 4-core Intel Xeon @ 2.10GHz, clang-18, Release (`-O3 -DNDEBUG`),
portable reflection backend, 100k particles unless stated. Run-to-run spread on
this (shared, 4-core) box is roughly ±10%, so every timing below is a median of
three runs and no claim rests on a difference smaller than that. Work-item
counts are exact — they are structural, not timed.

---

## 1. How FMM maps onto the library

| FMM stage | its shape | ECS construct | fit |
|---|---|---|---|
| particle storage | SoA over N | components in one archetype | **native** |
| Morton key | per-row map | `add_parallel`, `Query<Pos const, Key>` | **native** |
| spatial sort | permute rows by key | `Commands::sort<Key>` (`World::sort_rows`) | **native** |
| tree build | recursive, irregular | `add_serial` writing a `Tree` resource | serial only |
| interaction lists | recursive dual-tree | same serial system | serial only |
| P2M | gather a leaf's contiguous particle rows | `add_parallel` over *leaf driver rows* + `WorldView` columns | needs driver entities |
| M2M / L2L | per level, gather across one level | one `add_parallel` **per level** + `phase{}` | needs driver entities, manual order |
| M2L | node ← many nodes, any level | one `add_parallel` over all node rows | needs the write escape hatch |
| L2P + P2P | per particle, gather other particles | `add_parallel` `Query<Field, …>` + `WorldView` | **native** (with the trick in §3) |
| integrate | per-row map | `add_parallel` | **native** |
| render handoff | snapshot | `TripleBuffer` / `SnapshotChannel` | **native** |

The three constructs that carry it are not obscure — they are the ones the
library's own `static_assert` recommends for pairwise work ("build a spatial
structure … read it via `Res<T>`"), plus `WorldView` for ad-hoc reads.
`examples/mist` already does the one-level version of this: its `grid` system is
a P2M into cells and `steer` reads the neighbourhood back. FMM is that pattern
made hierarchical.

## 2. What the library gives you for free

These are real advantages over rolling the loop by hand, and they are why the
answer is "yes" rather than "use a task framework":

1. **The spatial sort is already a first-class, deterministic operation.**
   `World::sort_rows<C>(key)` counting-sorts rows by a compact integral key and
   patches every entity record; `Commands::sort` defers it to a barrier. FMM's
   central data-layout precondition — *a box's particles are a contiguous row
   range* — is one line, and it is stable, so the permutation is identical at
   any lane count.
2. **Dispatch is cheap enough that 25 barriers per tick do not matter.** The
   persistent spin-wait pool costs ~1–3 µs per wave (measured: `m2m.0` runs 1.4
   µs of work in a 2.1 µs wave), so the whole 25-wave tick spends well under
   0.1 ms in barriers. FMM's phase-per-level structure is affordable.
3. **Dynamic item claiming balances irregular work well** when there are enough
   items: `evaluate` (L2P + P2P, per-row cost varying by an order of magnitude)
   hit **3.83× on 4 lanes**, which is essentially the machine.
4. **Zero-allocation steady state.** With node pools that only grow, a tick
   records no commands and allocates nothing; `prewarm()` pre-pays the first
   tick.
5. **Per-system attribution is built in.** `ScheduleReport` gave every number in
   §5 with no instrumentation of my own — including work-item counts, which is
   how the granularity problem in §4.4 became visible at all.
6. **Reflection handles the coefficient types.** Verified: a component holding
   `std::array<double, 26>` is one column and round-trips; `std::complex<double>`
   works as an opaque single field; a raw C array (`double c[26]`) is rejected
   with a named diagnostic telling you to wrap it in `std::array`.

## 3. The caveat that decides the design

**A parallel system can only see its own slice, and can only write its own
rows.** `Query::for_each_chunk` iterates `item_.units` — the rows the executor
handed this item — so an M2L kernel cannot reach the source node's coefficients
through its query, and a leaf-driven P2P cannot write the particles' fields.

There are exactly two ways out, and a real FMM uses both:

- **`WorldView::for_each_chunk<Cs...>`** hands back every matching archetype's
  **full contiguous column spans**, read-only. Capture them once at the top of a
  body and you have O(1) random access into the real SoA columns — no shadow
  copy. This is how the spike's P2P reads source positions and charges. Cost:
  `WorldView` declares *reads everything*, so the system serializes against
  every writer in its phase.
- **A resource.** The tree and the coefficient arrays live in resources, read
  through `Res<T>`. Writing them is the problem — see §4.3.

The consequence is architectural: **tree nodes become entities that carry
nothing but a slot index.** Row count is the only unit of parallelism the
executor has, so "one entity per node" is the only way a per-node pass reaches
the lanes at all. The payload lives in the resource; the entity is a handle for
the scheduler. In the spike these are pooled and never destroyed — the tree is
rebuilt every tick, but the *rows* are not, so structural churn is zero in
steady state.

## 4. Friction, ranked by how much it costs you

### 4.1 The tree build and traversal cannot be parallelized at all — **highest**

A recursive build/traversal has no row shape, so it is an `add_serial` system:
one opaque work item, one lane. There is no nested dispatch either (the pool
throws on re-entrancy by design), so a serial system cannot fan out internally.

Measured, and flat across lane counts (1 lane / 4 lanes):

| leaf_cap | nodes | `build` | share of the 4-lane tick |
|---|---|---|---|
| 32 | 6205 | 2.88 / 2.84 ms | 1.6% |
| 4 | 23531 | 11.9 / 11.9 ms | 6.7% |

At 4 lanes this is nearly free; it is the **scaling** limit. Holding the
measured serial part fixed and dividing the rest (`leaf_cap=4`: 178 ms at 4
lanes, of which ~12 ms is serial), the tick would be ~56 ms at 16 lanes (75% of
ideal) and ~25 ms at 64 (41%) — and that ignores §4.4, which makes it worse.
Past roughly 16 lanes you are timing the tree build. *That extrapolation is
arithmetic on measured serial fractions, not a measurement.*

*Fixes, in order of cost:* rebuild the tree every k ticks and only re-sort in
between (`every{n}` is already a registration option, and this is standard
practice for slowly-evolving distributions); or express the traversal as a
level-by-level expansion where the *pairs* are rows — which is expressible here,
but only by making pairs entities, i.e. hundreds of thousands of spawns per
tick.

### 4.2 Ordering has to be done by hand — **high**

The scheduler derives ordering from declared access. FMM's coefficient writes
are invisible to it (§4.3), so the passes are ordered with `phase{}`: one phase
per level for M2M, one for M2L, one per level for L2L. 25 systems, 25 waves,
every edge hand-placed. Get one phase number wrong and you get a silently wrong
answer, not a diagnostic — precisely the failure mode the library's derived
access is designed to prevent.

Note the one place it *does* work: `assign-leaf` writes `LeafId`, `evaluate`
reads it, both in the same phase, and the scheduler levels them itself. That is
the model working as advertised — it just cannot reach the passes whose state
lives in a resource.

There is a way to recover more of it that the spike deliberately does not take:
give each level its **own resource type** (`MpAt<L>`, `LcAt<L>`) and write them
with `Extract`. Then M2M at level L declares `res_write(MpAt<L>)` and
`res_read(MpAt<L+1>)` and the upward pass orders itself. It breaks down on M2L,
whose dual-tree sources span *all* levels — that system would need one
`Res<MpAt<L>>` parameter per level (expressible with a pack-generated lambda,
but ~10 parameters and a runtime level→pointer table), and `Extract`'s target is
indexed by *row offset*, not node id, so the node arrays would have to be
re-indexed per level. Worth doing if strict declared access matters more than
the indexing complexity; not worth it otherwise.

### 4.3 Node-indexed writes have no declared form — **high**

Every FMM pass writes an array indexed by *node id* in pieces that are disjoint
but not row-aligned. The parallel-primitive suite covers every other shape:

| primitive | shape | why FMM doesn't fit |
|---|---|---|
| `Reduce<T,Op,P>` | fold into one target | writes are disjoint, not folded |
| `Extract<T>` | one output per row, at the item's row offset | output is indexed by node id, not row |
| `Collect<T>` | filtered append | order/indexing is not preserved |
| `Bin<V>` | group-by into buckets | rebuilt per tick from emitted pairs |
| `ResMut<T>` | mutable resource | **rejected in `add_parallel`** |

So the spike writes through a non-const pointer stored inside a resource read
via `Res<Coeffs>`. It is safe — the writes are provably disjoint — but it is
outside the model, and it is what forces §4.2.

*Suggested library change (small, and it would remove most of §4.2):* a
`Disjoint<T>` parallel parameter that declares a resource **write** exactly like
`Reduce`/`Extract` do, but binds a plain `T&`, on the body's promise that items
touch disjoint elements. That is `Extract` minus the row-alignment assumption —
same conflict analysis, same leveling, no new executor machinery.

### 4.4 The 1024-row work-item floor starves the tree passes — **medium**

`WavePlan::kMinItemRows = 1024` with a target of 64 items means a parallel
system gets `ceil(rows / max(1024, rows/64))` items — so **a pass with 1024 rows
or fewer gets exactly one item and runs on one lane**, however heavy each row
is. FMM's per-level passes are exactly that shape: 10³-flop rows, and few of
them.

Item counts at 100k, leaf_cap=32 (exact), against 4-lane speedup:

| system | rows | items | speedup at 4 lanes |
|---|---|---|---|
| `evaluate` | 100000 | 65 | 3.83× |
| `m2l` | 6205 | 7 | 2.95× |
| `m2m.*` (9 levels) | that level's nodes | 1–3 | 1.11× |
| `l2l.*` (9 levels) | that level's nodes | 1–3 | 1.19× |

Every `m2m`/`l2l` level except the deepest two is pinned to a single lane, which
is why those two rows are flat. Rebuilding with the floor lowered to 64 rows (a
one-constant patch, for this measurement only) turns them into 64-item systems.
At `leaf_cap=4`, 4 lanes, median of three runs each:

| pass | items (1024 floor → 64) | wall |
|---|---|---|
| `m2m.*` | 11 → 65 | 1.91 → **1.30 ms** |
| `l2l.*` | 11 → 65 | 3.54 → **1.94 ms** |
| `m2l` | 23 → 65 | 42.7 → 45.5 ms (no change — 23 items already exceeds 4 lanes) |
| whole tick | | 178 → 187 ms (within noise) |

So on *this* box the floor costs ~2 ms in a 178 ms tick — nothing. The reason it
still matters is that those passes are pinned at 1–11 items **regardless of lane
count**: on a 16- or 32-core host they cannot scale at all, and `m2l`'s 7 items
(leaf_cap=32) become a hard ceiling of 7 busy lanes.

*Suggested library change:* a `grain{n}` registration option next to
`phase`/`every`/`times`, overriding `kMinItemRows` per system. The floor is a
sensible default for cheap per-row work and simply the wrong default for a
kernel doing 10³ flops per row.

### 4.5 No query filters, so each pool needs its own component type — **low**

Matching is "archetype includes all requested components" with no `Without<>`,
so a `Query<NodeSlot const>` would also match the per-level pools if they shared
`NodeSlot`. Each pool therefore gets a distinct type (`LeafSlot`, `NodeSlot`,
`LevelSlot<L>`), and per-level passes become `LevelSlot<L>`-templated systems
registered over a compile-time `kMaxLevel` bound. It works (the spike registers
9 levels × 2 passes from an `integer_sequence`) but the max depth is baked in at
compile time, and every level costs two system registrations whether or not the
runtime tree ever reaches it. `Without<>` is already on the roadmap in
`IMPROVEMENTS.md` §6.1.

### 4.6 A body cannot learn its own global row index — **low**

Needed constantly in FMM (self-exclusion in P2P, mapping a row to a sorted-array
position). `Extract` knows the offset — it slices the target by it — but nothing
exposes it. The spike recovers it with pointer arithmetic on the entity spans
(`ents.data() - all_ents.data()`), which is sound but not obvious. Exposing
`item.begin` on the bound `Query` (e.g. `q.row_offset()`) would be a two-line
addition.

### 4.7 `WorldView` is all-or-nothing — **low**

It is the only route to other rows, and it declares *reads everything*, so any
system using it serializes against every writer in the phase. FMM's passes are
sequential anyway, so it costs nothing here — but it would in a schedule where
FMM runs alongside unrelated systems.

## 5. Measured scaling

100k particles, leaf_cap=32, clang-18 `-O3`, 4 cores. Each figure is the wave's
wall time as the scheduler measured it, median of three runs; the last column is
1-lane wall / 4-lane wall.

| system | items | 1 lane | 2 lanes | 4 lanes | 4-lane speedup |
|---|---|---|---|---|---|
| `morton` | 65 | 0.87 ms | 0.45 ms | 0.24 ms | 3.69× |
| `sort` (command flush) | 1 | 0.27 ms | 0.25 ms | 0.25 ms | 1.09× |
| `build` (serial) | 1 | 2.88 ms | 2.85 ms | 2.84 ms | 1.01× |
| `p2m` | 5 | 2.73 ms | 1.47 ms | 0.88 ms | 3.11× |
| `m2m.*` (9 waves) | 1–3 | 1.05 ms | 0.93 ms | 0.94 ms | 1.11× |
| `m2l` | 7 | 45.89 ms | 24.23 ms | 15.55 ms | 2.95× |
| `l2l.*` (9 waves) | 1–3 | 1.68 ms | 1.45 ms | 1.41 ms | 1.19× |
| `assign-leaf` | 65 | 1.71 ms | 0.82 ms | 0.43 ms | 3.99× |
| `evaluate` | 65 | 573.9 ms | 293.8 ms | 149.9 ms | 3.83× |
| **whole tick** | | **634 ms** | **326 ms** | **173 ms** | **3.66×** |

In the tree-heavy regime (`leaf_cap=4`: 23.5k nodes, 485k M2L pairs, 173k P2P
pairs) the tick goes 615 → 178 ms (**3.5×**), with `m2l` at 23 items and `build`
now 11.9 ms of serial floor.

Two things to read off this:

- **The parts the library is meant to parallelize, it parallelizes essentially
  perfectly** (3.83×/4 on the dominant pass, whose per-row cost varies by more
  than an order of magnitude — the dynamic claiming absorbs it).
- **What limits scaling is everything that isn't row-shaped**: the serial build,
  and the item-starved per-level passes. Both are addressable — one by cadence
  (§4.1), one by a grain option (§4.4).

Absolute performance is not the point of the spike (the kernels are unoptimized
`std::complex<double>` with a `log()` per direct pair, and the near field is
computed with the potential, which a force-only sim would skip). The *shape* is
the point.

## 6. If you build this for real

Recommended architecture — it is what the spike converged on:

1. **Particles are entities.** `Pos`/`Charge`/`Field`/`Key` as components, one
   archetype, sorted by Morton key with `Commands::sort` every tick.
2. **The tree and the coefficients are resources.** Node ids assigned
   level-major so a level is a contiguous id range.
3. **Node entities are pooled row-drivers**, one pool per pass shape
   (leaves, all-nodes, per-level), each with its own slot component type. Grow
   only; never destroy.
4. **Reads go through `Res<Tree>`/`Res<Coeffs>`; particle reads go through
   `WorldView` column spans.** No shadow copies.
5. **Order with `phase{}`**, one per level for the upward and downward passes,
   one shared phase for all M2L levels (dual-tree M2L has no cross-level
   dependency, so a single system over all nodes gives the best load balance
   available).
6. **Rebuild the tree on a cadence** (`every{n}`), re-sorting rows every tick.

Library changes worth making first, in value order:

| change | effort | buys |
|---|---|---|
| `grain{n}` registration option (§4.4) | ~10 lines | per-level passes scale past 1 lane |
| `Disjoint<T>` parallel param (§4.3) | ~40 lines | declared writes ⇒ automatic ordering, kills the escape hatch |
| `q.row_offset()` (§4.6) | ~2 lines | removes a pointer-arithmetic workaround |
| `Without<C>` filter (§4.5, already roadmap) | moderate | one slot component instead of one type per pool |

## 7. What this does not answer

- **3D.** The kernels get bigger (spherical harmonics, ~189-box V-lists) and the
  arithmetic intensity per node rises — which makes §4.4 *more* important, not
  less. The ECS mapping is unchanged.
- **Beyond 4 cores.** Every lane-scaling claim past 4 is Amdahl arithmetic on
  measured serial fractions, not a measurement. The item-count ceilings (7 items
  for `m2l`) are the numbers to watch on a bigger host.
- **Distributed or GPU FMM.** The library is a single-process, CPU, shared-memory
  executor; neither is in scope for it.
- **The P2996 reflection backend** (needs GCC 16) — the spike was built on the
  portable backend only.
- **A full time-stepping simulation.** The spike computes forces; integration
  and the render handoff are the parts of the library that already have examples.
