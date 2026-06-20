// examples/particles/gl_util.hpp
//
// Small OpenGL/GLFW helpers: building a program from shader files, dumping the
// framebuffer for the capture path, and window callbacks. This is also the one
// place the GL and GLFW headers are included (with the macOS deprecation
// silenced), so the rest of the example just includes this.
#pragma once

#define GL_SILENCE_DEPRECATION // macOS ships OpenGL as deprecated-but-present
#define GLFW_INCLUDE_NONE      // we include the GL header (gl3.h) ourselves, so
                               // include order can't pull in the legacy gl.h
#include <GLFW/glfw3.h>
#include <OpenGL/OpenGL.h> // CGL: context locking for multithreaded rendering
#include <OpenGL/gl3.h>    // Apple's core-profile GL entry points (no loader)
#include <string_view>
#include <vector>

namespace glutil {

// Read the two GLSL files, compile and link them into a program. Returns 0 and
// logs to stderr on any failure (missing file, compile error, link error).
GLuint load_program(char const* vert_path, char const* frag_path);

// Dump an RGBA framebuffer (as returned by glReadPixels) to a 24-bit BMP.
void write_bmp(char const* path, int w, int h, unsigned char const* rgba);

// GLFW key callback: close the window on Esc. (Framebuffer-resize handling
// lives in main.cpp, where it hands the new size to the render thread.)
void key_cb(GLFWwindow* win, int key, int scancode, int action, int mods);

// A minimal bitmap-font text overlay (5x7 glyphs, no texture): draws short ASCII
// strings as solid quads in framebuffer-pixel space -- enough for the on-screen
// FPS and particle-count readouts. Supports digits, the uppercase letters in
// "FPS"/"PARTICLES", space, ':' and '.'. Unknown characters render blank.
// Every method requires a current GL context; call destroy() before the context
// is released. Drawn opaque, so it stays readable over the additive scene.
class TextOverlay {
public:
    // Load the overlay shaders and create GL buffers. Returns false (and the
    // object stays inert) on failure.
    bool init(char const* vert_path, char const* frag_path);
    // Release GL objects. Safe to call when uninitialized.
    void destroy();

    // Draw `text` with its top-left at (x, y) in framebuffer pixels, each glyph
    // cell `px` pixels, in colour (r, g, b), for an fb_w x fb_h framebuffer.
    void draw(std::string_view text,
              float x,
              float y,
              float px,
              int fb_w,
              int fb_h,
              float r,
              float g,
              float b);

private:
    GLuint prog_ = 0, vao_ = 0, vbo_ = 0;
    GLint u_fb_ = -1, u_color_ = -1;
    std::vector<float> verts_; // reused scratch buffer for the quad stream
};

} // namespace glutil
