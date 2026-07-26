// web/dynamic/parallel_smoke.cpp
//
// Stretch-phase spike: can a JS system run *in parallel* on the WorkerPool's Web
// Worker lanes? Emscripten pthreads are Web Workers with separate JS contexts, so
// a main-thread JS function can't be called on a worker. The route is:
//
//   * ship the kernel as SOURCE (a string), not a function (it must be pure --
//     no closures -- since it is re-created per worker);
//   * each lane eval()s it into its OWN JS context (cached per source), via an
//     EM_ASM that runs on that lane's thread;
//   * the kernel operates on a Float32 view over the SHARED wasm memory
//     (SharedArrayBuffer) at the column base, restricted to its slice [begin,end)
//     -- disjoint per lane, so writes never race.
//
// This proves the mechanism: a WorkerPool dispatch where each lane runs an
// eval'd JS kernel over its slice of a shared array. Built -pthread, run on node.

#include <ecs/parallel/worker_pool.hpp>

#include <cstdint>
#include <cstdio>
#include <emscripten.h>
#include <pthread.h>
#include <set>
#include <string>
#include <vector>

using ecs::parallel::WorkerPool;

int main() {
    constexpr std::size_t N = 4000;
    std::vector<float> arr(N);
    for (std::size_t i = 0; i < N; ++i)
        arr[i] = static_cast<float>(i);

    unsigned const lanes = 4;
    std::vector<std::uintptr_t> tids(lanes, 0); // which thread ran each slice

    // A pure JS kernel: x10 over [begin,end) of a Float32 view at byte offset
    // `base` into the shared heap. No closures -- only its arguments.
    std::string const src = "(function(begin,end,base){"
                            "var v=new Float32Array(HEAPF32.buffer,base,end);"
                            "for(var i=begin;i<end;i++)v[i]=v[i]*10.0;"
                            "})";

    auto const base = reinterpret_cast<std::uintptr_t>(arr.data());

    WorkerPool pool {lanes};
    pool.parallel_for(N, [&](std::size_t b, std::size_t e) {
        // Runs on each lane (main thread + workers). EM_ASM executes in THIS
        // lane's JS context; eval() defines the kernel there (cached per source).
        EM_ASM(
            {
                var src = UTF8ToString($0);
                var g   = globalThis;
                if (!g.__k)
                    g.__k = {};
                var fn = g.__k[src];
                if (!fn)
                    fn = g.__k[src] = eval(src);
                fn($1, $2, $3);
            },
            src.c_str(),
            static_cast<int>(b),
            static_cast<int>(e),
            static_cast<int>(base));
        tids[b * lanes / N] = reinterpret_cast<std::uintptr_t>(pthread_self());
    });

    bool ok = true;
    for (std::size_t i = 0; i < N; ++i)
        if (arr[i] != static_cast<float>(i) * 10.0f) {
            ok = false;
            std::printf("mismatch at %zu: got %f\n", i, arr[i]);
            break;
        }
    std::set<std::uintptr_t> const distinct(tids.begin(), tids.end());
    bool const pass = ok && distinct.size() >= 2;
    std::printf(
        "%s: %zu elements x10 via worker-eval'd JS kernels across %zu thread(s)\n",
        pass ? "PASS" : "FAIL",
        N,
        distinct.size());
    return pass ? 0 : 1;
}
