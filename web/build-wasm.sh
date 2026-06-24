#!/usr/bin/env bash
# Build the WebAssembly targets with a local (host) Emscripten SDK -- the emsdk
# checked out alongside this repo at ../emsdk (override with EMSDK_DIR). It drives
# CMake through the `wasm-config` preset, whose toolchain file points at the same
# ../emsdk -- so this is the exact configuration CLion uses for code intelligence
# and native builds of the web targets. See web/README.md.
#
#   web/build-wasm.sh                  # configure + build every target
#   web/build-wasm.sh build TARGET     # ... only TARGET (e.g. particles_js_mt)
#   web/build-wasm.sh run [SMOKE]      # build, then run a node smoke:
#                                      #   core (default) | dynamic | parallel
#   web/build-wasm.sh serve            # serve the demos over http (COOP/COEP)
#
# Override the serve port with PORT=9000.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-wasm"
PORT="${PORT:-8080}"
EMSDK_DIR="${EMSDK_DIR:-$(cd "$REPO/.." && pwd)/emsdk}"

# Most shells already have emcc on PATH via emsdk_env; otherwise activate the SDK.
if ! command -v emcc >/dev/null 2>&1; then
    [[ -f "$EMSDK_DIR/emsdk_env.sh" ]] || {
        echo "emcc not on PATH and no emsdk at $EMSDK_DIR (set EMSDK_DIR)"; exit 1; }
    # shellcheck disable=SC1091
    source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1
fi

build() {
    local target="${1:-}"
    local args=(-j)
    [[ -n "$target" ]] && args=(--target "$target" -j)
    echo ">> $(emcc --version | head -1)"
    ( cd "$REPO" && cmake --preset wasm-config && cmake --build "$BUILD_DIR" "${args[@]}" )
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
        node "$REPO/$BUILD_DIR/$smoke"
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
