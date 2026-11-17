# Parked: the JS bindings and browser demos

The JavaScript bindings and browser demos (`web/`) were removed from `main`
after commit **`fd5971c`**, while the core architecture changes on the way to
the first MVP. Keeping them working meant carrying every core change through a
second API surface, a second toolchain and a second threading model, for code
nothing else depends on.

This is a pause, not a removal. Runtime-defined content and an editor are
planned, and a JS host is expected to come back on top of the same
foundation. So the parts of the core that make that possible stay, and CI keeps
the core building and running as wasm.

## What was parked

All of it is intact at `fd5971c`:

| Path at `fd5971c` | What it was |
|---|---|
| `web/dynamic/bindings.cpp`, `web/dynamic/js_system.hpp` | The embind `DynamicWorld`: JS-defined components, JS systems (`defineSystem`), parallel JS kernels (`defineParallelSystem`), deferred `spawn`/`destroy` |
| `web/examples/` | Browser demos: the particle fountain in C++ and with JS systems, each single- and multi-threaded (WebGL2) |
| `web/test/smoke_dynamic.mjs`, `smoke_parallel.mjs`, `parallel_smoke.cpp` | Node smoke tests for the bindings and for worker-evaluated JS kernels |
| `web/build-wasm.sh`, `web/coi-server.py`, `web/CMakeLists.txt`, `web/README.md` | Local build script, a dev server with the cross-origin isolation headers threads need, the CMake, the docs |
| `web/docs/sim-on-worker-decoupling.md` | Design notes on running the simulation on a worker |

`docs/IMPROVEMENTS.md` §8 keeps the history and the roadmap for this code.

## What stayed, and why

- **The runtime-typed component module**: `include/ecs/dynamic/` (registry,
  `DynamicColumn`, `WorldOps`, `register_native`) and the declared-access system
  registration, `Schedule::add_dynamic_serial` / `add_dynamic_parallel`.
  Runtime-defined content and an editor build on these. They are tested
  natively (`tests/test_dynamic.cpp`, and the dynamic cases in
  `tests/test_schedule.cpp`) and on wasm (`tests/wasm/smoke.cpp`).
- **The core's wasm build**, checked in CI by two node smoke tests:
  - `tests/wasm/smoke.cpp` runs the single-threaded core and the runtime-typed
    module.
  - `tests/wasm/smoke_mt.cpp` runs a 4-lane `WorkerPool` on wasm threads and
    checks the results match a 1-lane run exactly.
- The `wasm-config` CMake preset, for building the core with a local
  Emscripten SDK.

## Connection points to preserve

A JS host, a scripting runtime and an editor all reach the engine through
these. Changing them is fine; removing them turns "bring the bindings back"
into "re-architect the engine".

1. **Type-erased column storage.** `IColumn` (`include/ecs/archetype.hpp`) lets
   an archetype hold columns whose type is only known at run time
   (`DynamicColumn`), and `IColumn::field_base(i)` gives raw access to any
   component's fields: what an inspector or a JS typed-array view reads.
2. **Runtime component identity.** Components are identified by `ComponentId`,
   with layouts described at run time (`dynamic/component_desc.hpp`,
   `dynamic/registry.hpp`), and `Signature`s are built from ids.
3. **Declared-access systems.** The scheduler orders systems by their
   `SystemAccess` (`schedule/access.hpp`), not by C++ types.
   `add_dynamic_serial` / `add_dynamic_parallel` register a declared access
   plus a type-erased `run(World&, Commands&, WorkItem const&)`, and `exclusive`
   covers systems whose access can't be analyzed.
4. **A World entry point for runtime operations.** `dynamic::WorldOps` is a
   friend of `World`, including the deferred `spawn_deferred` /
   `destroy_deferred` that runtime code uses mid-tick.

## Design constraints, so wasm stays reachable

- **No 64-bit size assumptions.** `size_t` and pointers are 32-bit on wasm32.
  Hashes pack into 64 bits before narrowing (see `detail/container/handle.hpp`
  and `archetype.hpp`).
- **The portable reflection backend must keep compiling.** Emscripten's clang
  has no P2996, so features that need it (such as `field_view`) stay optional.
- **Browser threading rules.** The main thread can't block (Emscripten
  busy-waits there instead of using `Atomics.wait`). Threads must be created up
  front (`-sPTHREAD_POOL_SIZE`). Shared memory needs cross-origin isolation.
  Growing memory with threads on is slow and invalidates JS typed-array views.
- **Some systems need to run on a specific thread.** Code that calls into a
  host language (JS, a scripting runtime, an editor's UI) may have to run on
  the thread that owns it. See known issue 1. This applies beyond wasm.
- **Exceptions.** The JS-facing API was built with `-fwasm-exceptions`. A JS
  exception that crosses C++ frames isn't reliably caught by `catch (...)`.

## What CI still checks

- Every native and sanitizer job runs `test_dynamic` and the dynamic cases in
  `test_schedule`.
- The `wasm (Emscripten)` job builds the core with the latest emsdk and runs
  `ecs_smoke.js` and `ecs_smoke_mt.js` under node.

## Known issues to fix when it comes back

1. **JS serial systems hang the threaded build when they share a wave with
   other work.** A worker lane can claim the JS system's work item, but its
   kernel is main-thread JS, so the call never returns and the pool's join
   waits forever. The code's comments said JS systems "run on the main thread";
   nothing enforced it, and it only held when the JS system was alone in its
   wave. A CI run reproduced it at `fd5971c` plus #78 (Actions run
   44035771312). Suggested fix: a per-system "run on the calling thread" flag on
   `detail::SystemRecord`, honored by `Wave::run` and set by the JS
   registration path.

   Reproducer, for `web/test/smoke_parallel.mjs`:

   ```js
   const w2 = new M.DynamicWorld();
   const Tag = w2.defineComponent('Tag', [{ name: 'v', type: 'f32' }]);
   const Busy = w2.defineComponent('Busy', [{ name: 'x', type: 'f32' }]);
   for (let i = 0; i < 4000; i++) { const e = w2.createEntity(); w2.addComponent(e, Busy, [i]); }
   w2.defineParallelSystem('busy', { write: [Busy], params: { k: 1 } },
     (lo, hi, c, p) => { for (let i = lo; i < hi; i++) c.Busy.x[i] += p.k; });
   for (let k = 0; k < 4; k++)
     w2.defineSystem(`e${k}`, { commands: true }, () => {
       for (let i = 0; i < 3; i++) w2.spawn([[Tag, [k * 100 + i]]]);
     });
   for (let t = 0; t < 5; t++) w2.tick();   // never returns today
   ```

2. **CI never ran `smoke_parallel.mjs`** (it was built but not run). Add it to
   the wasm job, with a timeout, when restoring.
3. **#79's wasm tests.** https://github.com/bvrooman/ecs/pull/79 added command
   order checks to `smoke_dynamic.mjs` and threaded emitter checks to
   `smoke_parallel.mjs`, then took them back out when `web/` was parked. They
   are in that PR's history (commits `0ef3bca` and `23b19e8`) and should come
   back with the bindings.

## How to bring it back

1. Restore the tree: `git checkout fd5971c -- web`.
2. In the root `CMakeLists.txt`, add `add_subdirectory(web)` alongside
   `tests/wasm` under `if (EMSCRIPTEN)`. In `web/CMakeLists.txt`, drop the
   `ecs_smoke` target, which now lives in `tests/wasm/`.
3. Port `web/dynamic/bindings.cpp` and `web/dynamic/js_system.hpp` to the
   current `Schedule` and `dynamic` APIs. They use only the connection points
   above.
4. Restore the wasm CI steps for the binding smokes, fix known issue 1, and
   bring back #79's tests.
