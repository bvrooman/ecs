// web/particles/main_web.cpp
//
// Browser (WebAssembly) front-end for the ECS particle fountain. The native
// renderer (examples/particles/main.cpp) is a three-thread design -- sim, render,
// and the main/event thread -- decoupled by a lock-free TripleBuffer and
// serialized against macOS window updates with the CGL lock. None of that maps to
// the web: WebGL is single-context and single-threaded, so here the whole thing
// collapses into one emscripten_set_main_loop callback that steps the simulation
// and draws the latest snapshot.
//
// Everything ECS is reused verbatim: the components/resources (particles.hpp) and
// the schedule itself (examples/particles/simulation.cpp, build_particle_schedule)
// are compiled unchanged. Only windowing and drawing are rewritten for GLFW's
// Emscripten port + WebGL2 (GLES 3.0). The on-screen FPS/count readout, drawn in
// GL natively, is published to the surrounding page's DOM instead.

#include "ecs/ecs.hpp"
#include "particles.hpp"
#include "simulation.hpp"

#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <emscripten.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <random>
#include <thread>

using namespace ecs;

// GLES 3.0 / WebGL2 ports of examples/particles/shaders/particle.{vert,frag}:
// `#version 300 es`, and the fragment shader needs an explicit float precision.
// gl_PointSize is honoured implicitly in GLES (no GL_PROGRAM_POINT_SIZE enable).
static char const* const kVertSrc = R"(#version 300 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = vec4(aPos, 0.0, 1.0);
    gl_PointSize = mix(2.0, 11.0, aColor.a); // bright sparks shrink as they die
}
)";

static char const* const kFragSrc = R"(#version 300 es
precision highp float;
in vec4 vColor;
out vec4 FragColor;
void main() {
    vec2 d = gl_PointCoord - vec2(0.5);
    float r = length(d) * 2.0;            // 0 at center .. 1 at edge
    float falloff = 1.0 - smoothstep(0.0, 1.0, r);

    float life = vColor.a;                // 1 when young .. 0 when old (age fade)
    vec3 col = vColor.rgb * vec3(1.0, mix(0.45, 1.0, life), mix(0.12, 1.0, life));
    col = mix(col, vec3(1.0), falloff * falloff * life); // white-hot young core
    FragColor = vec4(col, life * falloff);
}
)";

namespace {

GLuint compile_shader(GLenum type, char const* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link_program(char const* vs_src, char const* fs_src) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        std::fprintf(stderr, "program link error: %s\n", log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

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
    bool paused = false;

    std::unique_ptr<WorkerPool> pool; // null = inline; set in the -pthread build
    double tick_ms_ema = 0.0;         // smoothed schedule.run() cost, ms/tick

    double last_ms   = 0.0; // emscripten_get_now() of the previous frame
    double sim_accum = 0.0; // unspent real time, stepped out in fixed kDt ticks
    double fps_t0    = 0.0;
    long fps_frames  = 0;
};

App* g_app = nullptr;

void frame() {
    App& a = *g_app;

    // --- step the simulation in fixed kDt increments (decoupled from rAF rate) -
    double const now = emscripten_get_now();
    double dt        = (a.last_ms == 0.0) ? 0.0 : (now - a.last_ms) / 1000.0;
    a.last_ms        = now;
    if (dt > 0.25)
        dt = 0.25; // clamp after a tab-switch/stall so we don't spiral
    if (!a.paused) {
        a.sim_accum += dt;
        double const t0 = emscripten_get_now();
        int steps       = 0;
        for (; a.sim_accum >= cfg::kDt && steps < 8; ++steps) {
            // With a WorkerPool the data-parallel systems fan out across worker
            // lanes; without one the schedule runs inline on this thread.
            if (a.pool)
                a.schedule.run(a.world, *a.pool);
            else
                a.schedule.run(a.world);
            a.sim_accum -= cfg::kDt;
        }
        if (steps > 0) {
            double const per = (emscripten_get_now() - t0) / steps;
            a.tick_ms_ema = a.tick_ms_ema == 0.0 ? per : a.tick_ms_ema * 0.9 + per * 0.1;
        }
    }

    // --- match the viewport to the (possibly resized) canvas ------------------
    int w = 0, h = 0;
    glfwGetFramebufferSize(a.window, &w, &h);
    if (w != a.fbw || h != a.fbh) {
        a.fbw = w;
        a.fbh = h;
        glViewport(0, 0, w, h);
    }

    // --- draw the newest published snapshot -----------------------------------
    glUseProgram(a.program);
    glBindVertexArray(a.vao);
    if (a.snapshots->consume()) {
        RenderSnapshot const& snap = a.snapshots->front();
        a.count                    = static_cast<GLsizei>(snap.size());
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

    // --- publish FPS + live count to the page DOM (~every 0.4s) ---------------
    ++a.fps_frames;
    if (a.fps_t0 == 0.0)
        a.fps_t0 = now;
    double const el = (now - a.fps_t0) / 1000.0;
    if (el >= 0.4) {
        double const fps = a.fps_frames / el;
        a.fps_frames     = 0;
        a.fps_t0         = now;
        int const lanes = a.pool ? static_cast<int>(a.pool->lanes()) : 1;
        // clang-format off
        EM_ASM({
            var f = document.getElementById('fps');   if (f) f.textContent = $0.toFixed(0);
            var c = document.getElementById('count'); if (c) c.textContent = $1;
            var t = document.getElementById('tick');  if (t) t.textContent = $2.toFixed(3);
            var l = document.getElementById('lanes'); if (l) l.textContent = $3;
        }, fps, static_cast<int>(a.count), a.tick_ms_ema, lanes);
        // clang-format on
    }
}

} // namespace

// --- JS-callable control surface ------------------------------------------
// Exposed to the page (EMSCRIPTEN_KEEPALIVE -> Module._ecs_*) so the HTML
// controls can drive the live simulation. Each just pokes a resource or a flag
// on the running World. These run on the main thread between frames (JS is
// single-threaded, and the main thread is busy inside schedule.run during a
// dispatch), so never concurrently with a running system.
extern "C" {

EMSCRIPTEN_KEEPALIVE void ecs_set_gravity(float accel) {
    if (g_app)
        g_app->world.resource<Gravity>().accel = accel;
}

EMSCRIPTEN_KEEPALIVE void ecs_set_emit_rate(int per_tick) {
    if (g_app)
        g_app->world.resource<Emitter>().per_tick = per_tick;
}

// Nozzle position in clip space (-1..1), as handed over from a canvas pointer event.
EMSCRIPTEN_KEEPALIVE void ecs_set_nozzle(float clip_x, float clip_y) {
    if (!g_app)
        return;
    auto& e    = g_app->world.resource<Emitter>();
    e.origin_x = clip_x;
    e.origin_y = clip_y;
}

EMSCRIPTEN_KEEPALIVE void ecs_set_paused(int paused) {
    if (g_app)
        g_app->paused = (paused != 0);
}

// Destroy every live particle (they refill from the emitter on the next ticks).
EMSCRIPTEN_KEEPALIVE void ecs_reset() {
    if (!g_app)
        return;
    Schedule clear;
    clear.add_once("reset", [](Query<Position const> q, Commands& cmd) {
        q.each([&](Entity e, Position const&) { cmd.destroy(e); });
    });
    clear.run(g_app->world);
}

// Multi-threaded build only: pick the WorkerPool lane count live (1 = inline;
// higher rebuilds the resident worker set). A no-op in the single-threaded build.
EMSCRIPTEN_KEEPALIVE void ecs_set_lanes(int lanes) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    if (!g_app)
        return;
    lanes = lanes < 1 ? 1 : (lanes > 8 ? 8 : lanes);
    g_app->pool.reset(); // join the old workers first so the warm pool can refill
    if (lanes > 1)
        g_app->pool = std::make_unique<WorkerPool>(static_cast<unsigned>(lanes));
#else
    (void)lanes;
#endif
}

} // extern "C"

int main() {
    static App app;
    g_app = &app;

    // --- world + schedule (identical to the native example) -------------------
    app.world.emplace_resource<Gravity>(cfg::kGravity);
    // Fixed seed: deterministic, and avoids leaning on std::random_device quality
    // under wasm. The visual is plenty varied from the per-particle spread.
    app.world.emplace_resource<Rng>(Rng {std::mt19937 {0xC0FFEEu}});
    app.world.emplace_resource<Clock>(Clock {0.0f});
    app.world.emplace_resource<Emitter>(
        Emitter {cfg::kEmitPerTick, cfg::kOriginX, cfg::kOriginY});
    app.world.emplace_resource<TripleBuffer<RenderSnapshot>>();
    build_particle_schedule(app.schedule);
    app.snapshots = &app.world.resource<TripleBuffer<RenderSnapshot>>();
    std::printf("schedule: %zu systems across %zu parallel levels\n",
                app.schedule.size(),
                app.schedule.level_count());

#if defined(__EMSCRIPTEN_PTHREADS__)
    // Multi-threaded build: lane 0 is this (main) thread; lanes 1..N-1 are
    // resident Web Worker threads sharing memory via SharedArrayBuffer, and the
    // data-parallel field systems fan out across them each tick.
    {
        unsigned const hw    = std::thread::hardware_concurrency();
        unsigned const lanes = std::max(2u, std::min(4u, hw ? hw : 2u));
        app.pool             = std::make_unique<WorkerPool>(lanes);
        std::printf("execution: %u-lane WorkerPool (Web Worker threads)\n", lanes);
    }
#else
    std::printf("execution: inline (single-threaded)\n");
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

    app.program = link_program(kVertSrc, kFragSrc);
    if (!app.program)
        return 1;

    glGenVertexArrays(1, &app.vao);
    glGenBuffers(1, &app.vbo);
    glBindVertexArray(app.vao);
    glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(GpuParticle),
                          reinterpret_cast<void*>(offsetof(GpuParticle, x)));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(GpuParticle),
                          reinterpret_cast<void*>(offsetof(GpuParticle, r)));
    glEnableVertexAttribArray(1);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive: overlapping sparks glow

    emscripten_set_main_loop(frame, 0, 1); // 0 = drive from requestAnimationFrame
    return 0;
}
