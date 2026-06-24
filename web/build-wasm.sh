#!/usr/bin/env bash
# Build the WebAssembly targets inside the official emscripten/emsdk container,
# using it as a self-contained, reproducible compiler (nothing installed on the
# host). It drives CMake through the `wasm-config` preset (CMakePresets.json), so
# the exact configuration is reusable from CLion too -- with a Docker toolchain on
# the same image, the targets build natively from the IDE. See web/README.md.
#
#   web/build-wasm.sh                  # configure + build every target
#   web/build-wasm.sh build TARGET     # ... only TARGET (e.g. particles_js_mt)
#   web/build-wasm.sh run [SMOKE]      # build, then run a node smoke:
#                                      #   core (default) | dynamic | parallel
#   web/build-wasm.sh serve            # serve the demos over http (COOP/COEP)
#
# Override the toolchain image or the serve port:
#   EMSDK_IMAGE=emscripten/emsdk:latest web/build-wasm.sh
#   PORT=9000 web/build-wasm.sh serve
set -euo pipefail

IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:6.0.1}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-wasm"
PORT="${PORT:-8080}"

in_container() {
    docker run --rm -v "$REPO":/src -w /src "$IMAGE" bash -lc "$1"
}

build() {
    local target="${1:-}"
    local build_args="-j"
    [[ -n "$target" ]] && build_args="--target $target -j"
    echo ">> configuring (wasm-config preset) + building with $IMAGE"
    in_container "cmake --preset wasm-config && cmake --build $BUILD_DIR $build_args"
}

case "${1:-build}" in
    build)
        build "${2:-}"
        echo ">> built $BUILD_DIR/web/"
        ;;
    run)
        build
        case "${2:-core}" in
            core)     smoke="web/ecs_smoke.js" ;;
            dynamic)  smoke="web/smoke_dynamic.mjs" ;;
            parallel) smoke="web/smoke_parallel.mjs" ;;
            *) echo "unknown smoke '${2}' (use: core | dynamic | parallel)"; exit 1 ;;
        esac
        echo ">> running $smoke under node"
        in_container "node $BUILD_DIR/$smoke"
        ;;
    serve)
        DIR="$REPO/$BUILD_DIR/web"
        [[ -f "$DIR/particles.js" ]] || { echo "nothing built yet -- run: web/build-wasm.sh"; exit 1; }
        # COOP/COEP server so the multi-threaded pages' SharedArrayBuffer works;
        # the single-threaded pages are unaffected by the headers.
        echo ">> serving $DIR (cross-origin isolated)"
        echo ">>   native, single-threaded : http://localhost:$PORT/"
        echo ">>   native, multi-threaded   : http://localhost:$PORT/index-mt.html"
        echo ">>   JS-defined               : http://localhost:$PORT/index-js.html"
        echo ">>   JS-defined, parallel      : http://localhost:$PORT/index-js-mt.html"
        echo ">> Ctrl-C to stop"
        exec python3 "$REPO/web/coi-server.py" "$PORT" "$DIR"
        ;;
    *)
        echo "usage: web/build-wasm.sh [build [TARGET] | run [core|dynamic|parallel] | serve]"
        exit 1
        ;;
esac
