# ecs on the web (WebAssembly / Emscripten)

The engine is header-only C++; this directory builds it to WebAssembly and ships
a few demos. The build uses a **host Emscripten SDK** checked out alongside this
repo (`../emsdk`).

## Prerequisites

- **emsdk** at `../emsdk` (sibling of this repo). Recent enough for C++23 (the
  code uses deducing-`this`, so emscripten ≥ 3.1.6x / clang 18+); latest is fine:
  ```sh
  git clone https://github.com/emscripten-core/emsdk.git ../emsdk
  (cd ../emsdk && ./emsdk install latest && ./emsdk activate latest)
  ```
  (Installed elsewhere? Set `EMSDK_DIR` for the script and edit `toolchainFile`
  in `CMakePresets.json` → `wasm-config`.)
- **cmake** and **node** on `PATH` (Homebrew cmake + any recent node work).

## Demos

```sh
web/build-wasm.sh          # build every target
web/build-wasm.sh serve    # serve at http://localhost:8080  (Ctrl-C to stop)
```

The server sets the COOP/COEP headers the multi-threaded pages need for
`SharedArrayBuffer`.

| URL | simulation defined in | threading |
| --- | --- | --- |
| `/` | C++ | single |
| `/index-mt.html` | C++ | parallel (`WorkerPool`) |
| `/index-js.html` | **JavaScript** | single |
| `/index-js-mt.html` | **JavaScript** | **parallel data systems across worker lanes** |

Headless node smokes:

```sh
web/build-wasm.sh run            # core engine smoke (ecs_smoke)
web/build-wasm.sh run dynamic    # JS-defined components/systems (smoke_dynamic.mjs)
web/build-wasm.sh run parallel   # defineParallelSystem across lanes (smoke_parallel.mjs)
```

## Building a specific target

`build-wasm.sh` drives CMake through the `wasm-config` preset, so it takes a
target:

```sh
web/build-wasm.sh build particles_js_mt
```

Targets: `ecs_smoke`, `particles_web`, `particles_web_mt`, `ecs_dynamic`,
`ecs_dynamic_mt`, `particles_js`, `particles_js_mt`, `ecs_parallel_smoke`.

## Source layout

- `web/examples/` — the browser demos. `particles/` and `particles_js/` are the
  C++ render hosts; `public/` holds the served assets (the four HTML pages, shared
  `demo.css`, shared JS — `controls.js` for the C++ demos, `fountain.js` for the
  JS demos — and the per-page bootstraps `app-*.js`); `shaders/` holds the GLSL,
  `--embed-file`'d into the GL wasm and read via the shared `gl_util.hpp`.
  `public/` is copied wholesale to the build/serve dir.
- `web/test/` — headless node smokes: `smoke.cpp` (core engine), `parallel_smoke.cpp`
  (worker-eval spike), `smoke_dynamic.mjs`, `smoke_parallel.mjs`.
- `web/dynamic/` — the embind runtime: `bindings.cpp` (the `DynamicWorld` module,
  shared by the smokes and the JS demos) and the `js_system.hpp` helper.
- `web/docs/` — design notes. [`sim-on-worker-decoupling.md`](docs/sim-on-worker-decoupling.md):
  a deferred proposal to run the JS fountain's sim on its own worker thread for true
  render-independence — covers the closure constraint, the architecture, and feasibility.

## CLion

### Code intelligence for the web targets (resolve, no false errors)

The `web/` targets are behind `if(EMSCRIPTEN)` in the root `CMakeLists.txt`, so
the **native** CMake profile never configures them — CLion then treats their
sources as orphan files and flags every emscripten/ecs include as unresolved. The
fix is to also load the **WASM** profile:

1. *Settings → Build, Execution, Deployment → CMake* → enable a profile from the
   **`wasm-config`** preset (CLion reads it from `CMakePresets.json`).
2. **Apply.** CLion configures it with the `../emsdk` toolchain, so the web
   targets enter the project model and the emscripten + ecs headers resolve.

When you edit a file under `web/`, CLion uses that profile and the false errors
clear. (A couple of things stay fuzzy for any indexer — the `EM_ASM` body is
stringified JS, and some embind templates are deep — but the includes and
references resolve.) The same profile lets you build/run the WASM targets from
the toolbar.

This is a plain CMake toolchain-file profile (no Docker toolchain), so it works
on both the classic and Nova engines.

### Run configurations

Shell-script run configs (`.idea/runConfigurations/`, local) drive the build with
one click — they run in the IDE terminal so `emcc`/`cmake`/`node` resolve from
your shell profile:

- `wasm: build` / `wasm: build particles_js` / `wasm: build particles_js_mt`
- `wasm: serve demo` — serves all four pages (URLs printed; click to open)
- `wasm: smoke test (node)` / `wasm: dynamic smoke (node)` / `wasm: parallel smoke (node)`
