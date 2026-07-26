// web/gl_util.hpp
//
// Shared WebGL helpers for the particle render hosts (particles/main_web.cpp and
// particles_js/host.cpp): compile/link a shader program, and read a shader source
// file embedded into the wasm via --embed-file (see web/CMakeLists.txt). Keeps the
// GLES boilerplate and the GLSL out of the C++.

#pragma once

#include <GLES3/gl3.h>
#include <cstddef>
#include <cstdio>
#include <string>

namespace web {

// Read an embedded file (e.g. "/shaders/particle.vert") into a string.
inline std::string read_file(char const* path) {
    std::string out;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "gl_util: cannot open %s\n", path);
        return out;
    }
    std::fseek(f, 0, SEEK_END);
    long const n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<std::size_t>(n));
        std::fread(out.data(), 1, out.size(), f);
    }
    std::fclose(f);
    return out;
}

inline GLuint compile_shader(GLenum type, char const* src) {
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

inline GLuint link_program(char const* vs_src, char const* fs_src) {
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
        return 0;
    }
    return p;
}

// Build a program from two embedded GLSL files (vertex, fragment).
inline GLuint link_program_files(char const* vs_path, char const* fs_path) {
    std::string const vs = read_file(vs_path);
    std::string const fs = read_file(fs_path);
    if (vs.empty() || fs.empty())
        return 0;
    return link_program(vs.c_str(), fs.c_str());
}

} // namespace web
