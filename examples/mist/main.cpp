// examples/mist/main.cpp
//
// A drifting cloud of black point-particles -- a fine mist that flows over a
// white page along an animated curl-noise field and parts around the cursor,
// driven entirely by the ECS library. The thread architecture mirrors the
// particle fountain (examples/particles): three threads, each a clear role, all
// decoupled by lock-free snapshots:
//
//   * sim    -- runs the Schedule at a fixed tick rate and publishes draw-ready
//               particles to a TripleBuffer;
//   * render -- owns the GL context, consumes the newest frame, and draws the
//               cloud as soft alpha-blended points at the display rate;
//   * main   -- owns the window, pumps events (required on macOS), and hands the
//               framebuffer size and the live cursor to the worker threads.
//
// Only the GL context moves off the main thread (GLFW allows that); the window
// and its event loop stay on main. See examples/particles/main.cpp for the full
// rationale on the CGL locking and per-thread QoS, which this reuses.
//
// Build (needs GLFW: `brew install glfw`); see examples/CMakeLists.txt.

#include "ecs/ecs.hpp"
#include "gl_util.hpp" // reused from examples/particles (identical GL/text helpers)
#include "mist.hpp"
#include "simulation.hpp"
#include "stats.hpp" // ecs::diag::TickStats -- per-tick timing distribution

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#ifndef MIST_SHADER_DIR
#define MIST_SHADER_DIR "examples/mist/shaders"
#endif

// Ask macOS to keep this thread on the performance cores (and at a higher
// priority), trimming the occasional multi-ms scheduler tail. No-op off macOS.
static void prefer_performance_cores() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// Drive the cursor along a scripted path (ECS_CURSOR_PATH) so the *follow*
// behaviour can be exercised headless -- something a static ECS_CURSOR cannot do.
// `t` is elapsed seconds; writes a clip-space (-1..1) point.
static void scripted_cursor(char const* path, double t, float& px, float& py) {
    if (std::strcmp(path, "step") == 0) {
        // Jump between the four quadrants every 4 s -- a step response (how fast,
        // and how closely, the flock reaches a freshly-placed cursor).
        static constexpr float wx[4] = {0.5f, -0.5f, -0.5f, 0.5f};
        static constexpr float wy[4] = {0.4f, 0.4f, -0.4f, -0.4f};
        int const k = int(t / 4.0) & 3;
        px = wx[k];
        py = wy[k];
    } else if (std::strcmp(path, "sweep") == 0) {
        px = 0.7f * float(std::sin(0.5 * t)); // horizontal back-and-forth
        py = 0.0f;
    } else { // "circle" (default): a steady orbit, for sustained tracking
        px = 0.6f * float(std::cos(0.6 * t));
        py = 0.6f * float(std::sin(0.6 * t));
    }
}

// The 3D view (camera, focal, clip/fade depths) lives in cfg (mist.hpp) so the
// simulation can share it for the cursor-predator projection; the renderer just
// feeds those constants to the shader below.

int main() {
    prefer_performance_cores(); // main / window-event thread

    // Mist density: many small particles read as a finer haze. ECS_PARTICLES
    // overrides the default for quick experimentation.
    int count = cfg::kCount;
    if (char const* p = std::getenv("ECS_PARTICLES"))
        if (int n = std::atoi(p); n > 0)
            count = n;

    // --- world + schedule + cross-thread channel --------------------------
    World world;
    // Seed: random by default, or ECS_SEED=<n> for a reproducible run (same
    // scatter + deterministic schedule => identical evolution, handy for
    // comparing the same flock at different timestamps).
    unsigned seed = std::random_device {}();
    if (char const* s = std::getenv("ECS_SEED"))
        if (unsigned v = unsigned(std::strtoul(s, nullptr, 10)); v != 0)
            seed = v;
    world.emplace_resource<Rng>(Rng {std::mt19937 {seed}});
    world.emplace_resource<Clock>(Clock {0.0f});
    world.emplace_resource<Cursor>(Cursor {});
    world.emplace_resource<Goal>(Goal {0.0f, 0.0f, 0.0f}); // wandering flock attractor
    world.emplace_resource<FlockGrid>(); // per-cell flock aggregates, rebuilt per tick
    world.emplace_resource<TripleBuffer<RenderSnapshot>>();

    WindowState wstate;

    Schedule schedule;
    build_mist_schedule(schedule,
                        MistInput {&wstate.cursor, &wstate.flags, &wstate.fb_size},
                        count);
    std::printf("mist: %d particles; schedule %zu systems across %zu levels\n",
                count,
                schedule.size(),
                schedule.level_count());

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

    GLFWwindow* window = glfwCreateWindow(900, 900, "ecs mist", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "failed to create window\n");
        glfwTerminate();
        return 1;
    }

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
    // Cursor handoff (main -> sim): publish the pointer position in clip space
    // and whether it is inside the window. These callbacks run on the main
    // thread during event polling and touch neither GL nor the ECS.
    glfwSetCursorPosCallback(window, [](GLFWwindow* win, double mx, double my) {
        auto* s = static_cast<WindowState*>(glfwGetWindowUserPointer(win));
        int w, h;
        glfwGetWindowSize(win, &w, &h); // logical-pixel window coords for the cursor
        float const nx = w ? float(mx) / float(w) * 2.0f - 1.0f : 0.0f;
        float const ny = h ? 1.0f - float(my) / float(h) * 2.0f : 0.0f; // flip Y
        s->cursor.store(pack_cursor(nx, ny), std::memory_order_relaxed);
    });
    glfwSetCursorEnterCallback(window, [](GLFWwindow* win, int entered) {
        auto* s        = static_cast<WindowState*>(glfwGetWindowUserPointer(win));
        std::uint32_t f = s->flags.load(std::memory_order_relaxed);
        f               = entered ? (f | 1u) : (f & ~1u);
        s->flags.store(f, std::memory_order_relaxed);
    });
    if (glfwGetWindowAttrib(window, GLFW_HOVERED))
        wstate.flags.store(1u, std::memory_order_relaxed);
    // ECS_CURSOR="x,y" seeds a fixed active cursor in clip space (-1..1) so the
    // avoided void is visible without moving the mouse (handy for a static
    // capture); live pointer motion still overrides it.
    if (char const* cs = std::getenv("ECS_CURSOR")) {
        float cx = 0.0f, cy = 0.0f;
        if (std::sscanf(cs, "%f,%f", &cx, &cy) == 2) {
            wstate.cursor.store(pack_cursor(cx, cy), std::memory_order_relaxed);
            wstate.flags.store(1u, std::memory_order_relaxed);
        }
    }
    glfwSetKeyCallback(window, glutil::key_cb);

    // `stop` ends both worker threads; the render thread also sets it (and wakes
    // main with glfwPostEmptyEvent) if GL initialization fails.
    std::atomic<bool> stop {false};
    std::atomic<long> ticks {0};
    std::atomic<long> rendered {0};

    // --- simulation thread: runs the schedule at a fixed tick rate ---------
    // Serial by default. ECS_POOL=<n> runs the schedule on an n-lane WorkerPool
    // (the `flow` curl-noise kernel is the part that scales). On an 8P+2E M1 Max
    // the live sweet spot is ~4 -- it must leave a P-core for the render and
    // main threads. ECS_STATS=1 prints per-tick timing every ~2s.
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
                run_tick();
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

        // The main thread calls [NSOpenGLContext update] on resize/move, which
        // touches this same context. Serialize every GL access behind the CGL
        // lock (that call takes it internally) or a concurrent resize crashes.
        CGLContextObj cgl = CGLGetCurrentContext();

        CGLLockContext(cgl);
        std::printf("GL %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
        GLuint program = glutil::load_program(MIST_SHADER_DIR "/mist.vert",
                                              MIST_SHADER_DIR "/mist.frag");
        if (!program) {
            CGLUnlockContext(cgl);
            glfwMakeContextCurrent(nullptr);
            stop.store(true, std::memory_order_release);
            glfwPostEmptyEvent(); // unblock the main event loop so it can exit
            return;
        }
        GLint const u_camz  = glGetUniformLocation(program, "uCamZ");
        GLint const u_focal = glGetUniformLocation(program, "uFocal");
        GLint const u_near  = glGetUniformLocation(program, "uNearClip");
        GLint const u_far   = glGetUniformLocation(program, "uFarFade");
        GLint const u_point = glGetUniformLocation(program, "uPointBase");

        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glVertexAttribPointer(0,
                              3,
                              GL_FLOAT,
                              GL_FALSE,
                              sizeof(GpuParticle),
                              (void*)offsetof(GpuParticle, x));
        glEnableVertexAttribArray(0);

        glEnable(GL_PROGRAM_POINT_SIZE); // let the vertex shader size points
        glEnable(GL_BLEND);
        // Standard "over": black ink (src) darkens the white page (dst), and
        // overlapping specks compound -- the misty density variation.
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // On-screen readout (a tiny bitmap-font overlay), drawn dark so it stays
        // legible over the white page. Optional: the demo runs without it.
        glutil::TextOverlay overlay;
        bool const has_overlay = overlay.init(MIST_SHADER_DIR "/text.vert",
                                              MIST_SHADER_DIR "/text.frag");
        if (!has_overlay)
            std::fprintf(stderr, "HUD overlay disabled (text shaders missing)\n");
        CGLUnlockContext(cgl);

        GLsizei count_pts       = 0;     // particles currently in the VBO
        long frames             = 0;
        std::uint64_t last_size = ~0ull; // force a glViewport on the first frame
        double fps        = 0.0;
        long fps_frames   = 0;
        auto fps_t0       = std::chrono::steady_clock::now();
        char fps_text[24] = "FPS 0.0"; // valid glyphs until the first 0.4s sample
        // Sim tick rate, measured over the same window from the shared counter
        // (holds at kSimHz while the schedule keeps up; dips if it overruns).
        char sim_text[24] = "SIM 0.0";
        long sim_last     = ticks.load(std::memory_order_relaxed);
        // Optional: ECS_CAPTURE=<path.bmp> grabs one frame after the cloud has
        // had a few seconds to organize (ECS_CAPTURE_AT=<seconds>, default 5),
        // then keeps running -- a GL readback of our own framebuffer. Time-gated
        // (not frame-gated) so the sim has evolved regardless of render rate.
        char const* capPath = std::getenv("ECS_CAPTURE");
        double cap_at       = 5.0;
        if (char const* a = std::getenv("ECS_CAPTURE_AT"))
            if (double v = std::atof(a); v > 0.0)
                cap_at = v;
        bool captured     = false;
        auto const loop_t0 = std::chrono::steady_clock::now();
        // ECS_CURSOR_PATH=circle|step|sweep drives the cursor over time (headless
        // follow tests). ECS_FILMSTRIP=<dir> grabs ECS_FILMSTRIP_N frames every
        // ECS_FILMSTRIP_EVERY seconds into <dir>/NN.bmp, to montage into a single
        // strip and review motion across time.
        char const* cursorPath = std::getenv("ECS_CURSOR_PATH");
        char const* filmDir    = std::getenv("ECS_FILMSTRIP");
        double film_every = 2.5, film_next = 0.0;
        int film_n = 12, film_shot = 0;
        if (char const* a = std::getenv("ECS_FILMSTRIP_EVERY"))
            if (double v = std::atof(a); v > 0.0)
                film_every = v;
        if (char const* a = std::getenv("ECS_FILMSTRIP_N"))
            if (int v = std::atoi(a); v > 0)
                film_n = v;

        while (!stop.load(std::memory_order_acquire)) {
            CGLLockContext(cgl); // serialize this frame against a resize/move update

            std::uint64_t sz = wstate.fb_size.load(std::memory_order_relaxed);
            int const fbw = int(sz >> 32), fbh = int(sz & 0xffffffffu);
            if (sz != last_size) {
                last_size = sz;
                glViewport(0, 0, fbw, fbh);
            }

            double const elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_t0).count();
            if (cursorPath) { // drive the scripted cursor (overrides live input)
                float px, py;
                scripted_cursor(cursorPath, elapsed, px, py);
                wstate.cursor.store(pack_cursor(px, py), std::memory_order_relaxed);
                wstate.flags.store(1u, std::memory_order_relaxed);
            }

            glUseProgram(program);
            glBindVertexArray(vao);
            // Fixed perspective view; base point size scales with the
            // framebuffer so the birds stay tiny but crisp at any resolution.
            if (u_camz >= 0)
                glUniform1f(u_camz, cfg::kCamZ);
            if (u_focal >= 0)
                glUniform1f(u_focal, cfg::kFocal);
            if (u_near >= 0)
                glUniform1f(u_near, cfg::kNearClip);
            if (u_far >= 0)
                glUniform1f(u_far, cfg::kFarFade);
            if (u_point >= 0)
                glUniform1f(u_point, std::max(1.0f, float(fbh) / 560.0f));

            // Pull the newest published snapshot, if any, and re-upload. "Latest
            // wins": when the 120 Hz sim outruns the display, frames are skipped.
            if (snapshots.consume()) {
                RenderSnapshot const& snap = snapshots.front();
                count_pts                  = static_cast<GLsizei>(snap.size());
                glBindBuffer(GL_ARRAY_BUFFER, vbo);
                glBufferData(GL_ARRAY_BUFFER,
                             snap.size() * sizeof(GpuParticle),
                             snap.data(),
                             GL_STREAM_DRAW);
            }

            glClearColor(1.0f, 1.0f, 1.0f, 1.0f); // white page
            glClear(GL_COLOR_BUFFER_BIT);
            if (count_pts > 0)
                glDrawArrays(GL_POINTS, 0, count_pts);

            // Update the FPS measurement every ~0.4s and draw the HUD top-left.
            ++fps_frames;
            auto const fnow     = std::chrono::steady_clock::now();
            double const fps_el = std::chrono::duration<double>(fnow - fps_t0).count();
            if (fps_el >= 0.4) {
                fps        = fps_frames / fps_el;
                fps_frames = 0;
                std::snprintf(fps_text, sizeof(fps_text), "FPS %.1f", fps);
                long const sim_now = ticks.load(std::memory_order_relaxed);
                std::snprintf(sim_text, sizeof(sim_text), "SIM %.1f",
                              double(sim_now - sim_last) / fps_el);
                sim_last = sim_now;
                fps_t0   = fnow;
            }
            if (has_overlay) {
                float const s = std::max(2.0f, fbh / 320.0f); // glyph cell, px
                char count_text[28], time_text[24];
                std::snprintf(count_text, sizeof(count_text), "PARTICLES %d", int(count_pts));
                // Elapsed sim time (ticks x dt) -- matches ECS_CAPTURE_AT and the
                // goal's phase, so behaviour can be read against a timestamp.
                double const sim_time =
                    double(ticks.load(std::memory_order_relaxed)) * double(cfg::kDt);
                std::snprintf(time_text, sizeof(time_text), "TIME %.1f", sim_time);
                // FPS = render frames/s, SIM = schedule ticks/s, TIME = sim secs.
                overlay.draw(fps_text, 2.0f * s, 2.0f * s, s, fbw, fbh, 0.1f, 0.1f, 0.1f);
                overlay.draw(sim_text, 2.0f * s, 11.0f * s, s, fbw, fbh, 0.1f, 0.1f, 0.1f);
                overlay.draw(time_text, 2.0f * s, 20.0f * s, s, fbw, fbh, 0.1f, 0.1f, 0.1f);
                overlay.draw(count_text, 2.0f * s, 29.0f * s, s, fbw, fbh, 0.35f, 0.35f, 0.4f);
            }

            // Grab one frame after the cloud has had cap_at seconds to organize.
            double const cap_el = std::chrono::duration<double>(fnow - loop_t0).count();
            if (capPath && !captured && count_pts > 0 && cap_el >= cap_at) {
                std::vector<unsigned char> px(std::size_t(fbw) * fbh * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glutil::write_bmp(capPath, fbw, fbh, px.data());
                captured = true;
                std::printf("captured frame -> %s (%dx%d, %d particles)\n",
                            capPath,
                            fbw,
                            fbh,
                            int(count_pts));
                std::fflush(stdout);
            }

            // Filmstrip: grab successive frames into <dir>/NN.bmp at a fixed
            // cadence, to montage into one image and read the motion over time.
            if (filmDir && film_shot < film_n && count_pts > 0 && elapsed >= film_next) {
                char fpath[512];
                std::snprintf(fpath, sizeof(fpath), "%s/%02d.bmp", filmDir, film_shot);
                std::vector<unsigned char> px(std::size_t(fbw) * fbh * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, fbw, fbh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                glutil::write_bmp(fpath, fbw, fbh, px.data());
                ++film_shot;
                film_next += film_every;
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
    while (!glfwWindowShouldClose(window) && !stop.load(std::memory_order_acquire))
        glfwWaitEvents();
    stop.store(true, std::memory_order_release);

    render.join(); // releases the GL context before we terminate GLFW
    sim.join();

    glfwDestroyWindow(window);
    glfwTerminate();

    std::printf("rendered %ld frames; simulated %ld ticks (%zu particles)\n",
                rendered.load(),
                ticks.load(),
                world.size());
    return 0;
}
