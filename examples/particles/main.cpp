// examples/particles/main.cpp
//
// A minimal OpenGL particle effect (a glowing fountain) driven entirely by the
// ECS library. Three threads, each a clear role, all decoupled by snapshots:
//
//   * sim    -- runs the Schedule at a fixed tick rate on a std::execution
//               thread pool and publishes draw-ready particles to a lock-free
//               TripleBuffer;
//   * render -- owns the GL context, consumes the newest frame, and draws the
//               latest snapshot as additive point sprites at the display rate;
//   * main   -- owns the window and pumps events (required on macOS) and hands
//               the framebuffer size to the render thread.
//
// GLFW's threading model is what makes this legal: window/event functions are
// main-thread only, but the context functions (make-current, swap, swap
// interval) may run on any one thread. So only the GL context moves; the window
// and its event loop stay on main. A bonus on macOS: because rendering no longer
// shares the main thread, the fountain keeps animating during a live window
// resize (which spins a nested Cocoa modal loop that blocks main).
//
// Build (needs GLFW: `brew install glfw`); see examples/CMakeLists.txt.

#include <ecs/ecs.hpp>
#include "gl_util.hpp"
#include "particles.hpp"
#include "simulation.hpp"
#include <ecs/diag/stats.hpp> // ecs::diag::TickStats -- per-tick timing distribution

#if PARTICLES_DEV_TOOLING
#include <ecs/viz/schedule_viz.hpp> // dev profile: render the assembled schedule to SVG
#include <fstream>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h> // pthread_set_qos_class_self_np
#include <sys/qos.h> // QOS_CLASS_USER_INTERACTIVE
#endif

using namespace ecs;

// Absolute path to the shaders directory, baked in by CMake so the app finds
// them regardless of the working directory it is launched from.
#ifndef PARTICLES_SHADER_DIR
#define PARTICLES_SHADER_DIR "examples/particles/shaders"
#endif

// Main->render handoff. The framebuffer-size callback runs on the main thread
// (during event polling) but must not touch GL, so it just stores the size; the
// render thread reads it and calls glViewport. Width and height are packed into
// one atomic so the render thread never reads a half-updated pair.
struct WindowState {
    std::atomic<std::uint64_t> fb_size {0}; // (uint64(w) << 32) | uint32(h)
};
static std::uint64_t pack_size(int w, int h) {
    return (std::uint64_t(std::uint32_t(w)) << 32) | std::uint32_t(h);
}

// Ask macOS to keep this thread on the performance cores (and at a higher
// priority), which trims the occasional multi-ms tail from the scheduler parking
// the sim/render thread on an efficiency core. No-op off macOS.
static void prefer_performance_cores() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

int main() {
    prefer_performance_cores(); // main / window-event thread

    // --- world + schedule + cross-thread channel --------------------------
    World world;
    world.emplace_resource<Gravity>(cfg::kGravity);
    world.emplace_resource<Rng>(Rng {std::mt19937 {std::random_device {}()}});
    world.emplace_resource<Clock>(Clock {0.0f});
    world.emplace_resource<Emitter>(
        Emitter {cfg::kEmitPerTick, cfg::kOriginX, cfg::kOriginY});
    world.emplace_resource<TripleBuffer<RenderSnapshot>>();

    Schedule schedule;
    build_particle_schedule(schedule);
    std::printf("schedule: %zu systems across %zu parallel levels\n",
                schedule.size(),
                schedule.level_count());

#if PARTICLES_DEV_TOOLING
    // Dev profile: dump the assembled schedule's wave DAG (with reflected
    // component/resource names) so it can be inspected without running the sim.
    if (std::ofstream svg {"particles_schedule.svg"}) {
        svg << viz::to_svg(schedule);
        std::printf("[dev] wrote schedule DAG -> particles_schedule.svg\n");
    }
#endif

    // The consumer side of the triple buffer. Resolved before the producer
    // starts; consume()/front() are only ever called on the render thread.
    auto& snapshots = world.resource<TripleBuffer<RenderSnapshot>>();

    // --- GLFW window setup (main thread; context is NOT made current here) -
    glfwSetErrorCallback([](int code, char const* desc) {
        std::fprintf(stderr, "GLFW error %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE); // required on macOS
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(900, 900, "ecs particles", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }

    WindowState wstate;
    {
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        wstate.fb_size.store(pack_size(w, h), std::memory_order_relaxed);
    }
    glfwSetWindowUserPointer(window, &wstate);
    glfwSetFramebufferSizeCallback(window, [](GLFWwindow* win, int w, int h) {
        auto* s = static_cast<WindowState*>(glfwGetWindowUserPointer(win));
        s->fb_size.store(pack_size(w, h), std::memory_order_relaxed);
    });
    glfwSetKeyCallback(window, glutil::key_cb);

    // `stop` ends both worker threads; the render thread also sets it (and wakes
    // main with glfwPostEmptyEvent) if GL initialization fails.
    std::atomic<bool> stop {false};
    std::atomic<long> ticks {0};
    std::atomic<long> rendered {0};

    // --- simulation thread: runs the schedule at a fixed tick rate ---------
    // Execution mode: serial (a 1-lane pool) by default. ECS_POOL=<n> runs the
    // schedule on an n-lane WorkerPool (the sim thread is lane 0, plus n-1
    // resident workers), splitting each for_each_chunk system across lanes.
    // benchmarks/schedule_bench shows it scaling ~5x with a tight tail. The
    // resident lanes spin while idle -- kept hot so a dispatch starts with zero
    // wakeup latency (parking them between ticks instead was measured far worse
    // for this fixed-timestep loop: the per-tick wakeup cost dwarfed the saving)
    // -- so an n-lane pool keeps n cores continuously busy. n must therefore
    // leave a performance core for each other hot thread (the render and main
    // threads here), not merely fit the core count: on this 8 P-core + 2 E-core
    // M1 Max, n=4 is the measured sweet spot, while n=8 oversubscribes the
    // P-cores and the fork-join barrier stalls on a descheduled lane. ECS_STATS=1
    // prints per-tick timing every ~2s.
    int pool_n = 0; // 0 = serial (1 lane)
    if (char const* p = std::getenv("ECS_POOL"))
        pool_n = std::atoi(p);
    bool const stats_on = diag::env_enabled("ECS_STATS");
    if (pool_n <= 0)
        std::printf("sim execution: inline\n");
    else
        std::printf("sim execution: %d-lane WorkerPool (data-parallel)\n", pool_n);

    std::thread sim([&] {
        prefer_performance_cores();
        using clock       = std::chrono::steady_clock;
        auto const period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(cfg::kDt));
        auto loop = [&](auto&& run_tick) {
            diag::TickStats tick_stats; // reports the distribution ~every 2s
            auto next = clock::now();
            while (!stop.load(std::memory_order_acquire)) {
                auto const a = clock::now();
                run_tick(); // emits, simulates, publishes
                ticks.fetch_add(1, std::memory_order_relaxed);
                if (stats_on) {
                    tick_stats.sample(
                        std::chrono::duration<double, std::micro>(clock::now() - a).count());
                    if (auto const s = tick_stats.due()) {
                        std::printf("%s\n", diag::TickStats::format(*s, "sim tick").c_str());
                        std::fflush(stdout);
                    }
                }
                next += period;
                std::this_thread::sleep_until(next); // pace to kSimHz
            }
        };
        WorkerPool pool {unsigned(std::max(1, pool_n))}; // 1 lane = serial
        loop([&] { schedule.run(world, pool); });
    });

    // --- render thread: owns the GL context and the draw loop --------------
    std::thread render([&] {
        prefer_performance_cores();
        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // vsync: channel skips frames the display can't show

        // The main thread calls [NSOpenGLContext update] when the window resizes
        // or moves, touching the same context this thread renders with. Serialize
        // every access behind the CGL lock -- [NSOpenGLContext update] takes the
        // same lock internally -- or a concurrent resize/move crashes or logs a
        // fatal GL error. Released between frames so a pending update can get in.
        CGLContextObj cgl = CGLGetCurrentContext();

        CGLLockContext(cgl);
        std::printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        GLuint program = glutil::load_program(PARTICLES_SHADER_DIR "/particle.vert",
                                              PARTICLES_SHADER_DIR "/particle.frag");
        if (!program) {
            CGLUnlockContext(cgl);
            glfwMakeContextCurrent(nullptr);
            stop.store(true, std::memory_order_release);
            glfwPostEmptyEvent(); // unblock the main event loop so it can exit
            return;
        }

        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0,
                              2,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(GpuParticle),
                              (void*)offsetof(GpuParticle, x));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1,
                              4,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(GpuParticle),
                              (void*)offsetof(GpuParticle, r));
        glEnableVertexAttribArray(1);

        glEnable(GL_PROGRAM_POINT_SIZE); // let the vertex shader size points
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive: overlapping sparks glow

        // On-screen FPS counter (a tiny bitmap-font overlay). If its shaders
        // fail to load, the example still runs -- just without the readout.
        glutil::TextOverlay overlay;
        bool const has_overlay = overlay.init(PARTICLES_SHADER_DIR "/text.vert",
                                              PARTICLES_SHADER_DIR "/text.frag");
        if (!has_overlay)
            std::fprintf(stderr, "FPS overlay disabled (text shaders missing)\n");
        CGLUnlockContext(cgl);

        GLsizei count           = 0; // particles currently in the VBO
        long frames             = 0;
        std::uint64_t last_size = ~0ull; // force a glViewport on the first frame
        // FPS measured over a short window so the displayed value stays steady.
        double fps        = 0.0;
        long fps_frames   = 0;
        auto fps_t0       = std::chrono::steady_clock::now();
        char fps_text[24] = "FPS --";
        // Optional: ECS_CAPTURE=<path.bmp> grabs one frame once the fountain has
        // filled, then keeps running -- a GL readback of our own framebuffer.
        char const* capPath = std::getenv("ECS_CAPTURE");
        bool captured       = false;

        while (!stop.load(std::memory_order_acquire)) {
            CGLLockContext(cgl); // serialize this frame against a resize/move update

            // Match the viewport to the latest framebuffer size handed over by
            // the main thread's resize callback.
            std::uint64_t sz = wstate.fb_size.load(std::memory_order_relaxed);
            if (sz != last_size) {
                last_size = sz;
                glViewport(0, 0, int(sz >> 32), int(sz & 0xffffffffu));
            }

            // The text overlay leaves its own program/VAO bound, so re-select
            // the particle ones each frame.
            glUseProgram(program);
            glBindVertexArray(vao);

            // Pull the newest published snapshot, if any, and re-upload. "Latest
            // wins": when the 120 Hz sim outruns the display, intermediate frames
            // are simply skipped.
            if (snapshots.consume()) {
                RenderSnapshot const& snap = snapshots.front();
                count                      = static_cast<GLsizei>(snap.size());
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER,
                             snap.size() * sizeof(GpuParticle),
                             snap.data(),
                             GL_STREAM_DRAW);
            }

            glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (count > 0)
                glDrawArrays(GL_POINTS, 0, count);

            // Update the FPS measurement every ~0.4s and draw it top-left.
            ++fps_frames;
            auto const fnow     = std::chrono::steady_clock::now();
            double const fps_el = std::chrono::duration<double>(fnow - fps_t0).count();
            if (fps_el >= 0.4) {
                fps        = fps_frames / fps_el;
                fps_frames = 0;
                fps_t0     = fnow;
                std::snprintf(fps_text, sizeof(fps_text), "FPS %.1f", fps);
            }
            if (has_overlay) {
                int const fbw = int(sz >> 32), fbh = int(sz & 0xffffffffu);
                float const s = std::max(2.0f, fbh / 320.0f); // glyph cell, px
                char count_text[28];
                std::snprintf(count_text, sizeof(count_text), "PARTICLES %d", int(count));
                // FPS in white, live particle count in the fountain's warm tint.
                overlay.draw(fps_text, 2.0f * s, 2.0f * s, s, fbw, fbh, 1.0f, 1.0f, 1.0f);
                overlay.draw(count_text,
                             2.0f * s,
                             11.0f * s,
                             s,
                             fbw,
                             fbh,
                             1.0f,
                             0.78f,
                             0.35f);
            }

            // Grab one frame once the fountain has actually filled (count-based,
            // not frame-based, so it works regardless of sim/render pacing).
            if (capPath && !captured && frames >= 60 && count > 3000) {
                // Framebuffer size from the atomic, not glfwGetFramebufferSize:
                // that is a main-thread-only call.
                int cw = int(sz >> 32), ch = int(sz & 0xffffffffu);
                std::vector<unsigned char> px(std::size_t(cw) * ch * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glutil::write_bmp(capPath, cw, ch, px.data());
                captured = true;
                std::printf("captured frame -> %s (%dx%d, %d particles)\n",
                            capPath,
                            cw,
                            ch,
                            int(count));
                std::fflush(stdout);
            }

            glfwSwapBuffers(window);
            CGLUnlockContext(cgl);
            ++frames;
        }
        rendered.store(frames, std::memory_order_relaxed);

        // GL teardown happens here, where the context is current; then release
        // it so the main thread may terminate GLFW.
        CGLLockContext(cgl);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
        overlay.destroy();
        CGLUnlockContext(cgl);
        glfwMakeContextCurrent(nullptr);
    });

    // --- main event loop ---------------------------------------------------
    // Window and event handling must stay on the main thread. Block until the
    // window is closed (Esc/close button) or the render thread bails out.
    while (!glfwWindowShouldClose(window) && !stop.load(std::memory_order_acquire))
        glfwWaitEvents();
    stop.store(true, std::memory_order_release);

    render.join(); // releases the GL context before we terminate GLFW
    sim.join();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("rendered %ld frames; simulated %ld ticks (%zu particles left)\n",
                rendered.load(),
                ticks.load(),
                world.size());
    return 0;
}
