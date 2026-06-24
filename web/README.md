# ecs on the web (WebAssembly / Emscripten)

The engine is header-only C++; this directory builds it to WebAssembly and ships
a few demos. The toolchain lives entirely in the official `emscripten/emsdk`
Docker image — nothing is installed on the host.

## Demos

Build, then serve them (the server sets the COOP/COEP headers the multi-threaded
pages need for `SharedArrayBuffer`):

```sh
web/build-wasm.sh          # build every target
web/build-wasm.sh serve    # serve at http://localhost:8080  (Ctrl-C to stop)
```

| URL | simulation defined in | threading |
| --- | --- | --- |
| `/` | C++ | single |
| `/index-mt.html` | C++ | parallel (`WorkerPool`) |
| `/index-js.html` | **JavaScript** | single |
| `/index-js-mt.html` | **JavaScript** | **parallel data systems across worker lanes** |

Headless node smokes (no browser):

```sh
web/build-wasm.sh run            # core engine smoke (ecs_smoke)
web/build-wasm.sh run dynamic    # JS-defined components/systems (smoke_dynamic.mjs)
web/build-wasm.sh run parallel   # defineParallelSystem across lanes (smoke_parallel.mjs)
```

## Building a specific target

`build-wasm.sh` drives CMake through the `wasm-config` preset, so it accepts a
target:

```sh
web/build-wasm.sh build particles_js_mt
```

Targets: `ecs_smoke`, `particles_web`, `particles_web_mt`, `ecs_dynamic`,
`ecs_dynamic_mt`, `particles_js`, `particles_js_mt`, `ecs_parallel_smoke`.

## CLion

**Run configurations** (under `.idea/runConfigurations/`, shell-script type) drive
the Docker build with one click — they run in the IDE terminal so `docker` and
`python3` resolve from your shell profile:

- `wasm: build` / `wasm: build particles_js` / `wasm: build particles_js_mt`
- `wasm: serve demo` — serves all four pages (URLs printed; click to open)
- `wasm: smoke test (node)` / `wasm: dynamic smoke (node)` / `wasm: parallel smoke (node)`

**Native CMake targets via a Docker toolchain** — to build the WASM targets from
CLion's own CMake (so they appear in the target dropdown and build from the
toolbar), point CLion at the same container:

1. *Settings → Build, Execution, Deployment → Toolchains* → **+** → **Docker** →
   image `emscripten/emsdk:6.0.1`.
2. *Settings → Build, Execution, Deployment → CMake* → **+** → select the
   **`wasm-config`** preset and assign the Docker toolchain from step 1.
3. The `wasm-config` profile now lists every WASM target; pick one and build/run
   from the toolbar.

The preset's `toolchainFile` is a path *inside* the container, so `wasm-config`
only configures within the emsdk image (the Docker toolchain, or `build-wasm.sh`)
— not against the host toolchain.

## Requirements

Docker (Desktop) running. The image is pinned to `emscripten/emsdk:6.0.1`;
override with `EMSDK_IMAGE=…`, or the serve port with `PORT=…`.
