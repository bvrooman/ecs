// web/particles/main_web.cpp
//
// Browser (WebAssembly) front-end for the ECS particle fountain. WebGL is
// single-context and lives on the main (browser) thread, so rendering always runs
// there, from the requestAnimationFrame callback (frame()).
//
// Single-threaded build: the sim is stepped inside that same rAF callback, so the
// sim and render rates are coupled.
//
// Multi-threaded build (-pthread): the sim runs on its own Web Worker thread,
// paced at a fixed kSimHz and fully decoupled from the render (rAF) rate -- the
// native three-thread design (examples/particles/main.cpp) adapted to the web's
// GL-on-main constraint. The sim worker is the sole writer of the World and the
// sole producer for a lock-free TripleBuffer (which lives in SharedArrayBuffer);
// the main thread only consumes the newest snapshot and draws it. Live controls
// cross from the main thread to the sim as atomics, applied once per tick
// (apply_inputs), so the World keeps a single writer.
//
// Everything ECS is reused verbatim: the components/resources (particles.hpp) and
// the schedule itself (examples/particles/simulation.cpp, build_particle_schedule)
// are compiled unchanged. Only windowing and drawing are rewritten for GLFW's
// Emscripten port + WebGL2 (GLES 3.0). The on-screen FPS/count readout, drawn in
// GL natively, is published to the surrounding page's DOM instead.

#include "../../dynamic/js_system.hpp"
#include "../gl_util.hpp"
#include <ecs/dynamic/native.hpp>
#include <ecs/ecs.hpp>
#include "particles.hpp"
#include "simulation.hpp"
#include "stats.hpp" // ecs::diag::TickStats (tools/, via the ecs::diag include path)
#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <memory>
#include <random>
#include <thread>

using namespace ecs;

namespace {

// All sim + render state, owned for the lifetime of the main loop. A single
// instance is kept in a static so the C-style main-loop callback can reach it.
struct App {
    World world;
    Schedule schedule;
    TripleBuffer<RenderSnapshot>* snapshots = nullptr;
    GLFWwindow* window                      = nullptr;
    GLuint program = 0, vao = 0, vbo = 0;
    GLsizei count = 0;
    int fbw = 0, fbh = 0;

    // Live controls. The JS-callable setters (main thread) write these atomics;
    // whichever thread owns the sim -- the main thread in the ST build, the sim
    // worker in the MT build -- copies them into the World each tick via
    // apply_inputs(). Keeping the World a single-writer is what makes the threaded
    // build safe: the main thread never touches the World while the sim runs.
    std::atomic<float> in_gravity {cfg::kGravity};
    std::atomic<int> in_emit {cfg::kEmitPerTick};
    std::atomic<float> in_nozzle_x {cfg::kOriginX};
    std::atomic<float> in_nozzle_y {cfg::kOriginY};
    std::atomic<bool> in_paused {false};
    std::atomic<bool> reset_req {false};
    std::atomic<int> desired_lanes {0}; // MT: >0 -> sim worker rebuilds its pool
    std::atomic<bool> stop {false};     // MT: ask the sim worker to exit

    // Sim -> render readouts for the DOM HUD (the sim writes, the main thread reads).
    std::atomic<double> tick_ms_ema {0.0}; // smoothed schedule.run() cost, ms/tick
    std::atomic<double> sim_hz {0.0};      // measured sim ticks/sec (decoupled rate)
    std::atomic<int> cur_lanes {1};

    bool stats_on = false;      // set once in main(); read by the ticking thread
    diag::TickStats tick_stats; // owned by the ticking thread (?stats)

    double last_ms   = 0.0; // ST build: emscripten_get_now() of the previous frame
    double sim_accum = 0.0; // ST build: unspent real time, stepped in fixed kDt ticks
    double fps_t0    = 0.0;
    long fps_frames  = 0;
};

App* g_app = nullptr;

// Copy the live-control atomics into the World and service deferred requests
// (reset). Runs on the thread that owns the sim (main in ST, the worker in MT),
// so the World is only ever written from one thread.
void apply_inputs(App& a) {
    a.world.resource<Gravity>().accel = a.in_gravity.load(std::memory_order_relaxed);
    auto& e                           = a.world.resource<Emitter>();
    e.per_tick                        = a.in_emit.load(std::memory_order_relaxed);
    e.origin_x                        = a.in_nozzle_x.load(std::memory_order_relaxed);
    e.origin_y                        = a.in_nozzle_y.load(std::memory_order_relaxed);
    if (a.reset_req.exchange(false, std::memory_order_acq_rel)) {
        Schedule clear;
        clear.add_once("reset", [](Query<Position const> q, Commands& cmd) {
            q.for_each([&](Entity en, auto&) { cmd.destroy(en); });
        });
        clear.run(a.world);
    }
}

void frame() {
    App& a           = *g_app;
    double const now = emscripten_get_now();

#if !defined(__EMSCRIPTEN_PTHREADS__)
    // Single-threaded build: step the sim right here, paced by accumulated real
    // time. (The MT build runs the sim on its own worker instead; see main().)
    double dt = (a.last_ms == 0.0) ? 0.0 : (now - a.last_ms) / 1000.0;
    a.last_ms = now;
    if (dt > 0.25)
        dt = 0.25; // clamp after a tab-switch/stall so we don't spiral
    if (!a.in_paused.load(std::memory_order_relaxed)) {
        a.sim_accum += dt;
        int steps       = 0;
        double const t0 = emscripten_get_now();
        for (; a.sim_accum >= cfg::kDt && steps < 8; ++steps) {
            apply_inputs(a);
            double const tick_t0 = a.stats_on ? emscripten_get_now() : 0.0;
            a.schedule.run(a.world);
            a.sim_accum -= cfg::kDt;
            if (a.stats_on)
                a.tick_stats.sample((emscripten_get_now() - tick_t0) *
                                    1000.0); // ms -> us
        }
        if (steps > 0) {
            double const per  = (emscripten_get_now() - t0) / steps;
            double const prev = a.tick_ms_ema.load(std::memory_order_relaxed);
            a.tick_ms_ema.store(prev == 0.0 ? per : prev * 0.9 + per * 0.1,
                                std::memory_order_relaxed);
            if (a.stats_on)
                if (auto const s = a.tick_stats.due())
                    std::printf("%s\n", diag::TickStats::format(*s, "sim tick").c_str());
        }
    }
#endif

    // --- match the viewport to the (possibly resized) canvas ------------------
    int w = 0, h = 0;
    glfwGetFramebufferSize(a.window, &w, &h);
    if (w != a.fbw || h != a.fbh) {
        a.fbw = w;
        a.fbh = h;
        glViewport(0, 0, w, h);
    }

    // --- draw the newest published snapshot (the sim is its producer) ----------
    glUseProgram(a.program);
    glBindVertexArray(a.vao);
    if (a.snapshots->consume()) {
        auto const& snap = a.snapshots->front();
        a.count          = static_cast<GLsizei>(snap.size());
        glBindBuffer(GL_ARRAY_BUFFER, a.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     snap.size() * sizeof(GpuParticle),
                     snap.data(),
                     GL_STREAM_DRAW);
    }
    glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (a.count > 0)
        glDrawArrays(GL_POINTS, 0, a.count);

    // --- publish FPS + live count + (decoupled) sim-Hz to the page DOM --------
    ++a.fps_frames;
    if (a.fps_t0 == 0.0)
        a.fps_t0 = now;
    double const el = (now - a.fps_t0) / 1000.0;
    if (el >= 0.4) {
        double const fps = a.fps_frames / el;
        a.fps_frames     = 0;
        a.fps_t0         = now;
        // clang-format off
        EM_ASM({
            var f = document.getElementById('fps');   if (f) f.textContent = $0.toFixed(0);
            var c = document.getElementById('count'); if (c) c.textContent = $1;
            var t = document.getElementById('tick');  if (t) t.textContent = $2.toFixed(3);
            var l = document.getElementById('lanes'); if (l) l.textContent = $3;
            var s = document.getElementById('sim');   if (s) s.textContent = $4.toFixed(0);
        }, fps, static_cast<int>(a.count),
           a.tick_ms_ema.load(std::memory_order_relaxed),
           a.cur_lanes.load(std::memory_order_relaxed),
           a.sim_hz.load(std::memory_order_relaxed));
        // clang-format on
    }
}

#if defined(__EMSCRIPTEN_PTHREADS__)
// MT build: the simulation loop, run on its own Web Worker thread. Paced to a
// fixed kSimHz independent of the render (rAF) rate -- the point of the triple
// buffer. This thread is the sole writer of the World and the sole producer for
// the snapshot TripleBuffer; the main thread only consumes snapshots and draws.
void sim_thread_main() {
    App& a = *g_app;

    // Own the WorkerPool on this thread (the dispatching thread is lane 0), like
    // the native sim thread. Pre-warmed pthreads (PTHREAD_POOL_SIZE) back it.
    unsigned const hw    = std::thread::hardware_concurrency();
    unsigned const lanes = std::max(2u, std::min(4u, hw ? hw : 2u));
    auto pool            = std::make_unique<WorkerPool>(lanes);
    a.cur_lanes.store(static_cast<int>(pool->lanes()), std::memory_order_relaxed);
    std::printf("execution: %u-lane WorkerPool on a dedicated sim thread "
                "(sim %d Hz, render = rAF)\n",
                lanes,
                cfg::kSimHz);

    using clock       = std::chrono::steady_clock;
    auto const period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(cfg::kDt));
    auto next     = clock::now();
    auto hz_t0    = clock::now();
    long hz_ticks = 0;

    while (!a.stop.load(std::memory_order_acquire)) {
        // Live lane change requested from the UI -- applied here, since this thread
        // owns the pool (1 = inline, no pool).
        if (int const want = a.desired_lanes.exchange(0, std::memory_order_acq_rel);
            want > 0) {
            pool.reset(); // join the old workers before refilling the warm pool
            if (want > 1)
                pool = std::make_unique<WorkerPool>(static_cast<unsigned>(want));
            a.cur_lanes.store(pool ? static_cast<int>(pool->lanes()) : 1,
                              std::memory_order_relaxed);
        }

        if (!a.in_paused.load(std::memory_order_relaxed)) {
            apply_inputs(a);
            double const t0 = emscripten_get_now();
            if (pool)
                a.schedule.run(a.world, *pool);
            else
                a.schedule.run(a.world);
            double const ms   = emscripten_get_now() - t0;
            double const prev = a.tick_ms_ema.load(std::memory_order_relaxed);
            a.tick_ms_ema.store(prev == 0.0 ? ms : prev * 0.9 + ms * 0.1,
                                std::memory_order_relaxed);
            if (a.stats_on) {
                a.tick_stats.sample(ms * 1000.0); // ms -> us
                if (auto const s = a.tick_stats.due())
                    std::printf("%s\n", diag::TickStats::format(*s, "sim tick").c_str());
            }
            ++hz_ticks;
        }

        // Publish the measured tick rate (~every 0.5s) so the HUD can show the sim
        // running independently of the render frame rate.
        if (auto const e = clock::now() - hz_t0; e >= std::chrono::milliseconds(500)) {
            a.sim_hz.store(hz_ticks / std::chrono::duration<double>(e).count(),
                           std::memory_order_relaxed);
            hz_ticks = 0;
            hz_t0    = clock::now();
        }

        // Pace to kSimHz. If we fell more than a tick behind (a stall/tab-switch),
        // resync instead of spiral-spinning to catch up.
        next += period;
        if (auto const floor = clock::now() - period; next < floor)
            next = clock::now();
        std::this_thread::sleep_until(next);
    }
}
#endif

} // namespace

// --- JS-callable control surface ------------------------------------------
// Exposed to the page (EMSCRIPTEN_KEEPALIVE -> Module._ecs_*) so the HTML controls
// can drive the live simulation. Each only writes an atomic; the value is applied
// to the World by the thread that owns the sim (apply_inputs), so a control never
// races a running system -- in the ST build that thread is this one, in the MT
// build it is the sim worker.
extern "C" {

EMSCRIPTEN_KEEPALIVE void ecs_set_gravity(float accel) {
    if (g_app)
        g_app->in_gravity.store(accel, std::memory_order_relaxed);
}

EMSCRIPTEN_KEEPALIVE void ecs_set_emit_rate(int per_tick) {
    if (g_app)
        g_app->in_emit.store(per_tick, std::memory_order_relaxed);
}

// Nozzle position in clip space (-1..1), as handed over from a canvas pointer event.
EMSCRIPTEN_KEEPALIVE void ecs_set_nozzle(float clip_x, float clip_y) {
    if (!g_app)
        return;
    g_app->in_nozzle_x.store(clip_x, std::memory_order_relaxed);
    g_app->in_nozzle_y.store(clip_y, std::memory_order_relaxed);
}

EMSCRIPTEN_KEEPALIVE void ecs_set_paused(int paused) {
    if (g_app)
        g_app->in_paused.store(paused != 0, std::memory_order_relaxed);
}

// Destroy every live particle (they refill from the emitter on the next ticks).
// Deferred to the sim owner so it never races a running system.
EMSCRIPTEN_KEEPALIVE void ecs_reset() {
    if (g_app)
        g_app->reset_req.store(true, std::memory_order_release);
}

// Multi-threaded build only: pick the WorkerPool lane count live (1 = inline;
// higher rebuilds the resident worker set). The sim thread applies it. No-op
// single-threaded.
EMSCRIPTEN_KEEPALIVE void ecs_set_lanes(int lanes) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    if (!g_app)
        return;
    lanes = lanes < 1 ? 1 : (lanes > 8 ? 8 : lanes);
    g_app->desired_lanes.store(lanes, std::memory_order_release);
#else
    (void)lanes;
#endif
}

} // extern "C"

int main() {
    static App app;
    g_app = &app;
    // ?stats in the page URL -> log the per-tick timing distribution to the
    // browser console (no env var in a browser; this is the toggle).
    app.stats_on = EM_ASM_INT({ return location.search.indexOf('stats') >= 0 ? 1 : 0; });

    // --- world + schedule (identical to the native example) -------------------
    app.world.emplace_resource<Gravity>(cfg::kGravity);
    // Fixed seed: deterministic, and avoids leaning on std::random_device quality
    // under wasm. The visual is plenty varied from the per-particle spread.
    app.world.emplace_resource<Rng>(Rng {std::mt19937 {0xC0FFEEu}});
    app.world.emplace_resource<Clock>(Clock {0.0f});
    app.world.emplace_resource<Emitter>(Emitter {cfg::kEmitPerTick,
                                                 cfg::kOriginX,
                                                 cfg::kOriginY});
    app.world.emplace_resource<TripleBuffer<RenderSnapshot>>();
    build_particle_schedule(app.schedule);
    app.snapshots = &app.world.resource<TripleBuffer<RenderSnapshot>>();
    std::printf("schedule: %zu systems across %zu parallel levels\n",
                app.schedule.size(),
                app.schedule.level_count());

    // Expose the native particle components to the runtime layer so a JS-defined
    // system can read/write them by name (Phase C).
    dynamic::register_native<Position>("Position", {"x", "y"});
    dynamic::register_native<Velocity>("Velocity", {"x", "y"});
    dynamic::register_native<Color>("Color", {"r", "g", "b"});
    dynamic::register_native<Age>("Age", {"t", "max"});

#if !defined(__EMSCRIPTEN_PTHREADS__)
    std::printf("execution: inline (single-threaded)\n");
    // (MT build: the WorkerPool is created and owned by the sim thread, spawned
    // below once the schedule is final -- see sim_thread_main.)
#endif

    // --- GLFW (Emscripten port) + WebGL2 context ------------------------------
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SAMPLES, 4); // MSAA, best-effort via the WebGL2 context
    app.window = glfwCreateWindow(900, 900, "ecs particles", nullptr, nullptr);
    if (!app.window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(app.window);
    std::printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    app.program =
        web::link_program_files("/shaders/particle.vert", "/shaders/particle.frag");
    if (!app.program)
        return 1;

    glGenVertexArrays(1, &app.vao);
    glGenBuffers(1, &app.vbo);
    glBindVertexArray(app.vao);
    glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
    glVertexAttribPointer(0,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(GpuParticle),
                          reinterpret_cast<void*>(offsetof(GpuParticle, x)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(GpuParticle),
                          reinterpret_cast<void*>(offsetof(GpuParticle, r)));
    glEnableVertexAttribArray(1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive: overlapping sparks glow

    // Now that the world + schedule exist (g_app is set), let the page register
    // any JS-defined systems; they join the wave plan and run each tick. (The MT
    // page registers none -- JS-defined systems are main-thread closures and the
    // sim runs on a worker there; only the single-threaded page uses this.)
    EM_ASM({
        if (Module.onEcsReady)
            Module.onEcsReady();
    });

#if defined(__EMSCRIPTEN_PTHREADS__)
    // Hand the simulation to its own Web Worker thread, paced at kSimHz and fully
    // decoupled from the render rate; main() stays on the browser thread to drive
    // rAF rendering. Detached because emscripten_set_main_loop unwinds main().
    std::thread(sim_thread_main).detach();
#endif

    emscripten_set_main_loop(frame, 0, 1); // 0 = drive from requestAnimationFrame
    return 0;
}

// --- JS-system interop (Phase C PoC) --------------------------------------
// Exposed to the page via embind. ecs_components() hands JS the native component
// ids; ecs_define_system() registers a system, authored in JavaScript, into the
// live schedule -- where it runs each tick over the native particle components,
// leveled against the C++ systems by its declared access. (Single-threaded build
// only; the -pthread build runs the sim on a worker, which cannot call a
// main-thread JS closure.)
emscripten::val ecs_components() {
    using emscripten::val;
    val o = val::object();
    o.set("Position", static_cast<int>(component_id<Position>.value));
    o.set("Velocity", static_cast<int>(component_id<Velocity>.value));
    o.set("Color", static_cast<int>(component_id<Color>.value));
    o.set("Age", static_cast<int>(component_id<Age>.value));
    return o;
}

void ecs_define_system(std::string name, emscripten::val spec, emscripten::val kernel) {
    if (g_app)
        dynamic::web::add_js_system(g_app->schedule, std::move(name), spec, kernel);
}

EMSCRIPTEN_BINDINGS(particles_js) {
    emscripten::function("ecs_components", &ecs_components);
    emscripten::function("ecs_define_system", &ecs_define_system);
}
