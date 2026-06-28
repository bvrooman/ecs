# Proposal: decouple the JS fountain's sim from rendering (sim-on-a-worker)

**Status:** deferred / not started — captured for possible follow-up.
**Date:** 2026-06-25.
**Scope:** the JavaScript-defined fountain demos (`web/examples/public/app-js.js`,
`app-js-mt.js`) backed by `web/examples/particles_js/host.cpp` +
`web/dynamic/bindings.cpp` (`DynamicWorld`).

---

## 1. Background — two senses of "decoupling"

"Decouple the simulation frame rate from the rendering frame rate" can mean two
quite different things:

| | What it means | Where the sim runs | Status |
|---|---|---|---|
| **(A) Timestep decoupling** | Sim advances at a fixed simulated rate regardless of fps (a fixed-`dt` accumulator runs 0..N ticks per frame). | Still the main thread, inside the rAF callback. | **Done** — `app-js.js` / `app-js-mt.js` use a fixed-120 Hz accumulator. |
| **(B) Thread decoupling** | Sim runs on its **own thread**, genuinely unaffected by render hitches (the "throttle render to 19 fps and the sim holds 120 Hz" property). | A dedicated worker thread. | **This proposal.** |

For reference, the **C++** fountain host already has (B): `particles_web_mt` runs
the whole sim on a dedicated Web Worker thread (`sim_thread_main` in
`web/examples/particles/main_web.cpp`), paced at `cfg::kSimHz`, producing
snapshots into a `TripleBuffer<RenderSnapshot>` that the main thread draws. The
goal of this proposal is to bring that same thread independence to the **JS**
fountain.

(A) is genuinely useful and cheap, but it is **not** thread independence: the sim
still executes inside the rAF callback, so a slow render still steals its time and
a heavy sim still slows render. Under a hard render throttle, (A) degrades; (B)
does not. This proposal is only about (B).

## 2. Why (B) is hard for the JS path — the closure constraint

Each thread in the browser is a **separate JavaScript realm**: its own global
scope, its own heap of JS objects, its own functions. Threads share *raw bytes*
via `SharedArrayBuffer` (under `-pthread` the whole WASM heap is one), but **not
JS objects**.

A system registered with `defineSystem` is a JS *closure* — a JS object on the
heap of the thread that created it (the page's main thread), closing over
main-thread state (`w`, `RATE`, `Math.random`, the component ids). embind hands
C++ an `emscripten::val` handle to it, but a `val` is an index into a **per-thread**
handle table — valid only on the creating thread. So the C++ that invokes the
system must run on that thread. `emit` and `age` are `defineSystem`, so the tick
that runs them is pinned to the main thread.

`defineParallelSystem` escapes this precisely because it does **not** keep a
handle: it ships `fn.toString()` (the function's *source text* — shareable bytes),
and each worker lane `eval`s it into its own local copy. The price is that such a
kernel must be self-contained (no closing over main-thread state) and cannot
`spawn`/`destroy`.

**Consequence:** to move the sim onto a worker, the systems must be *defined on
that worker* (so their closures and val handles live there). The structural
systems (`emit`/`age`) can't simply be source-shipped today, because the
pure-kernel path has no `spawn`/`destroy`.

## 3. Proposed architecture

Run the **entire JS sim** — component schema, all systems, and the tick loop — in
a dedicated worker's JS realm, and reduce the main thread to a renderer:

```
sim worker (its own JS realm):
    define components + systems            ← the JS closures live HERE
    tick loop @ fixed kSimHz (its own clock, not rAF)
    pack [x, y, r, g, b] per live particle → shared triple-buffer (SharedArrayBuffer)

main thread (the page):
    rAF: read newest published snapshot from the buffer → glBufferData → draw
```

The seam between them is a triple buffer in shared memory — the same pattern the
C++ host uses, but straddling the worker/main boundary, and with the snapshot
*packed by JS on the worker* rather than by a C++ `extract` system.

### What splits, concretely

- **`fountain.js` splits.** `fountainComponents` + the `defineSystem` calls move
  to the worker script; `fountainRenderer`'s drawing stays on the page.
- **The renderer splits down the middle.** Today `fountainRenderer(w, C)` reads
  the ECS field views *and* draws on one thread. After the split, the **read/pack**
  half must be on the worker (only it can read its `DynamicWorld`'s field views)
  and the **upload+draw** half stays on the page (the GL context is there). The
  shared snapshot buffer is the new boundary between the halves.
- **The page becomes a thin renderer** — it no longer owns the world. The demo's
  pitch inverts: from "define your fountain on the page" to "the fountain *is* a
  worker; the page just blits a buffer."

## 4. Two implementation models

### Model 1 — dedicated Worker, own module instance (separate heap)

The worker `importScripts('particles-js-mt.js')` to load its **own** module
instance; the page allocates a `SharedArrayBuffer` and posts it over.

- **Pro:** clean separation; the "systems run where defined" story is obvious.
- **Con:** the worker's `DynamicWorld` is in *its own* heap, so the page can't read
  particle data directly — every tick must **copy** the packed snapshot into the
  shared `SharedArrayBuffer` (this we'd do anyway).
- **Con:** the worker still wants `defineParallelSystem` fan-out, so it needs its
  **own nested pool** of sub-workers (workers-within-a-worker). Supported, but
  another layer to stand up, and `-pthread`-from-a-Worker setup is fiddlier.

### Model 2 — emscripten pthread, shared heap

Make the sim a pthread of the *same* module (as the C++ host does). The heap is
shared, so the snapshot copy is trivial and the existing `WorkerPool` fan-out
works unchanged.

- **Pro:** shared memory; reuses the `WorkerPool`; closest to the C++ host design.
- **Con:** the page's `defineSystem` code isn't in the pthread's scope. To define
  the systems *on* the pthread you must ship their **source** and `eval` it there
  (the same source-not-closure trick), **and** extend that path so eval'd systems
  can `spawn`/`destroy`. So this trades the heap copy for "make all systems
  source-shippable and structural-capable" — new machinery in `bindings.cpp` /
  `js_system.hpp`.

## 5. Work breakdown

1. **Render-extract for the dynamic path.** A way to pack chosen component fields
   (`Position`, `Color`) into a contiguous `[x,y,r,g,b]` buffer. Either:
   - JS on the worker reads the `fieldView` typed arrays and writes the buffer, or
   - a small C++ helper on `DynamicWorld` that gathers named fields into a target
     buffer (faster, but the dynamic world is schema-generic so it needs to be told
     which fields are the render attributes).
2. **A JS/SAB triple buffer.** Three regions + an atomic "latest" index; producer
   writes `back`, publishes (atomic swap); consumer reads `front`. (Port of the C++
   `TripleBuffer` semantics into a `SharedArrayBuffer` layout.)
3. **The worker sim host.** A worker script (Model 1) or pthread entry (Model 2)
   that builds the world, defines the systems, runs a fixed-`kSimHz` loop, and
   publishes snapshots.
4. **The page renderer.** Strip sim-stepping from the JS render path; consume the
   latest snapshot and draw. Keep the existing GL bridge for the draw half.
5. **Bootstrapping + controls.** Coordinate startup (page waits for the worker,
   shares the SAB); route the existing live controls (gravity/emit/nozzle/pause)
   to the worker as atomics in shared memory (the C++ host's `apply_inputs`
   pattern), since they now cross a thread boundary.
6. **(Model 2 only)** Extend the source-shipped-kernel path to support
   `spawn`/`destroy` so `emit`/`age` can run on the worker.

## 6. Feasibility & effort

- **Feasible?** Yes. "Sim in a worker, snapshot over a shared buffer" is a standard
  game-engine pattern; nothing fundamental blocks it. The COOP/COEP isolation is
  already in place for the `_mt` demos.
- **Effort:** substantial — days, not minutes. It is a **re-architecture of the JS
  demo**, not a tweak: items 1–3 are new code (the dynamic path has no
  render-extract and no JS triple buffer today), and item 6 (Model 2) touches the
  parallel-system core.
- **Payoff (be honest):** render-independence for the JS fountain, plus a nice
  "throttle render, sim holds" demo. But it is **smaller than it was for C++**:
  even after the move, `emit`/`age` stay serial on the worker's main JS (only the
  data-parallel kernels parallelize, as they already do). And it **costs the demo's
  defining idea** — the fountain stops being "plain JS on your page."

## 7. Open questions / risks

- **Nested pthreads (Model 1):** spinning a `WorkerPool` from inside a Worker —
  browser + emscripten support and `PTHREAD_POOL_SIZE` accounting need validation.
- **Getting app JS into the pthread scope (Model 2):** loading the
  system-definition source into the pthread worker and `eval`ing it there is
  unusual emscripten territory; prototype it early to de-risk.
- **`spawn`/`destroy` from a source-shipped kernel (Model 2):** needs a command
  buffer accessible to the eval'd kernel — non-trivial.
- **Snapshot bandwidth:** at ~2.5k particles × 5 floats × 120 Hz the copy is
  cheap, but worth confirming it isn't the new bottleneck.
- **Determinism / input latency:** controls now apply a tick or two later (they
  cross to the worker) — acceptable for a fountain, but note it.

## 8. Recommendation

Keep **(A)** (shipped) as the default — it gives steady, fps-independent sim time
for ~5 lines and preserves the "define your fountain on the page" model. Pursue
**(B)** only if demonstrating *true thread independence on the JS path* is itself
the goal. If we do, **prototype Model 2** (shared heap) first behind the existing
`particles_js_mt` target — the heap sharing and `WorkerPool` reuse make it the
closest analogue to the working C++ host — and treat item 6 (`spawn`/`destroy` on
a source-shipped system) as the key feasibility spike.
