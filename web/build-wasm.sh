#!/usr/bin/env bash
# Build the WebAssembly targets inside the official emscripten/emsdk container,
# using it as a self-contained, reproducible compiler (nothing is installed on
# the host). Run from anywhere:
#
#   web/build-wasm.sh            # configure + build (ecs_smoke + particles)
#   web/build-wasm.sh run        # ... then run the headless smoke test in node
#   web/build-wasm.sh serve      # serve the built particle demo over http
#
# Override the toolchain image (e.g. to track latest), or the serve port:
#   EMSDK_IMAGE=emscripten/emsdk:latest web/build-wasm.sh
#   PORT=9000 web/build-wasm.sh serve
set -euo pipefail

# Pinned for reproducibility (emscripten 6.0.1: libc++ LLVM 21, node 22).
IMAGE="${EMSDK_IMAGE:-emscripten/emsdk:6.0.1}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="build-wasm"
PORT="${PORT:-8080}"

in_container() {
    docker run --rm -v "$REPO":/src -w /src "$IMAGE" bash -lc "$1"
}

build() {
    echo ">> configuring + building with $IMAGE"
    in_container "emcmake cmake -B $BUILD_DIR -S . \
            -DCMAKE_BUILD_TYPE=Release \
            -DECS_USE_P2996=OFF \
            -DECS_BUILD_TESTS=OFF -DECS_BUILD_EXAMPLES=OFF \
            -DECS_BUILD_BENCHMARKS=OFF -DECS_BUILD_TOOLS=OFF \
        && cmake --build $BUILD_DIR -j"
    echo ">> built $BUILD_DIR/web/ : ecs_smoke.js, particles.js (+ .wasm), index.html"
}

case "${1:-build}" in
    build)
        build
        ;;
    run)
        build
        echo ">> running headless smoke test under node"
        in_container "node $BUILD_DIR/web/ecs_smoke.js"
        ;;
    serve)
        DIR="$REPO/$BUILD_DIR/web"
        [[ -f "$DIR/particles.js" ]] || { echo "nothing built yet -- run: web/build-wasm.sh"; exit 1; }
        echo ">> serving $DIR"
        echo ">> open http://localhost:$PORT/  (Ctrl-C to stop)"
        exec python3 -m http.server "$PORT" --directory "$DIR"
        ;;
    *)
        echo "usage: web/build-wasm.sh [build|run|serve]"
        exit 1
        ;;
esac
