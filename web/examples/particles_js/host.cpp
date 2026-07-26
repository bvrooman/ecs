// web/particles_js/host.cpp
//
// A minimal WebGL2 host for a *fully JavaScript-defined* particle simulation. It
// owns only the GL context, the point-sprite shader, and a per-frame draw -- no
// components, no systems, no world. JavaScript (index-js.html) defines the
// components and systems on a DynamicWorld (the ecs_dynamic embind class, linked
// into this same module), steps them each frame, packs Position/Color/Age into a
// scratch buffer this host exposes as a zero-copy typed-array view, and calls
// draw_points(). The C++ here is the runtime + render glue only.

#include "../gl_util.hpp"
#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdio>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <vector>

namespace {

struct Host {
    GLFWwindow* window = nullptr;
    GLuint program = 0, vao = 0, vbo = 0;
    int fbw = 0, fbh = 0;
    double last_ms = 0.0;
    std::vector<float> scratch; // interleaved [x, y, r, g, b, life] per point
};
Host g;

void frame() {
    double const now = emscripten_get_now();
    double dt        = (g.last_ms == 0.0) ? 0.0 : (now - g.last_ms) / 1000.0;
    g.last_ms        = now;
    if (dt > 0.25)
        dt = 0.25;

    int w = 0, h = 0;
    glfwGetFramebufferSize(g.window, &w, &h);
    if (w != g.fbw || h != g.fbh) {
        g.fbw = w;
        g.fbh = h;
        glViewport(0, 0, w, h);
    }

    glClearColor(0.02f, 0.02f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Hand the frame to JS: it steps the JS-defined sim and calls draw_points().
    EM_ASM(
        {
            if (Module.onFrame)
                Module.onFrame($0);
        },
        dt);
}

} // namespace

// --- render bridge, exposed to JS -----------------------------------------
// (Re)size the interleaved scratch buffer to `points` and hand back a zero-copy
// Float32Array view (points * 6 floats: x, y, r, g, b, life) for JS to fill.
emscripten::val scratch_view(unsigned points) {
    g.scratch.resize(static_cast<std::size_t>(points) * 6);
    return emscripten::val(emscripten::typed_memory_view(g.scratch.size(),
                                                         g.scratch.data()));
}

// Upload the first `count` points from the scratch buffer and draw them.
void draw_points(unsigned count) {
    glUseProgram(g.program);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(count) * 6 * sizeof(float),
                 g.scratch.data(),
                 GL_STREAM_DRAW);
    if (count > 0)
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(count));
}

EMSCRIPTEN_BINDINGS(particles_js_host) {
    emscripten::function("scratch_view", &scratch_view);
    emscripten::function("draw_points", &draw_points);
}

int main() {
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SAMPLES, 4);
    g.window = glfwCreateWindow(900, 900, "ecs particles (JS-defined)", nullptr, nullptr);
    if (!g.window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(g.window);

    g.program =
        web::link_program_files("/shaders/particle.vert", "/shaders/particle.frag");
    if (!g.program)
        return 1;
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glVertexAttribPointer(0,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          6 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,
                          4,
                          GL_FLOAT,
                          GL_FALSE,
                          6 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive: overlapping sparks glow

    std::printf("particles_js host ready -- sim is defined in JavaScript\n");
    emscripten_set_main_loop(frame, 0, 1); // JS world is built in onRuntimeInitialized
    return 0;
}
