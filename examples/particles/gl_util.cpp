// examples/particles/gl_util.cpp
#include "gl_util.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace glutil {

namespace {

    // 5x7 bitmap font: each glyph is 7 rows, low 5 bits per row (bit 4 = leftmost
    // column). Just the characters the "FPS" and "PARTICLES" readouts need.
    struct Glyph {
        char c;
        std::uint8_t rows[7];
    };
    constexpr Glyph kFont[] = {
        {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
        {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
        {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
        {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
        {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
        {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
        {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
        {'A', {0x04, 0x0A, 0x11, 0x11, 0x1F, 0x11, 0x11}},
        {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
        {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
        {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04}},
    };
    std::uint8_t const* glyph_for(char c) {
        for (auto const& g : kFont)
            if (g.c == c)
                return g.rows;
        return kFont[0].rows; // unmapped -> blank
    }

    bool read_file(char const* path, std::string& out) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot open shader file: %s\n", path);
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    GLuint compile_shader(GLenum type, char const* src) {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::fprintf(stderr, "shader compile error: %s\n", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    }

    GLuint link_program(char const* vs_src, char const* fs_src) {
        GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
        GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
        if (!vs || !fs) {
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            return 0;
        }
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs);
        glAttachShader(prog, fs);
        glLinkProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
            std::fprintf(stderr, "program link error: %s\n", log);
            glDeleteProgram(prog);
            return 0;
        }
        return prog;
    }

} // namespace

GLuint load_program(char const* vert_path, char const* frag_path) {
    std::string vs, fs;
    if (!read_file(vert_path, vs) || !read_file(frag_path, fs))
        return 0;
    return link_program(vs.c_str(), fs.c_str());
}

bool TextOverlay::init(char const* vert_path, char const* frag_path) {
    prog_ = load_program(vert_path, frag_path);
    if (!prog_)
        return false;
    u_fb_    = glGetUniformLocation(prog_, "uFb");
    u_color_ = glGetUniformLocation(prog_, "uColor");
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    return true;
}

void TextOverlay::destroy() {
    if (vbo_)
        glDeleteBuffers(1, &vbo_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
    if (prog_)
        glDeleteProgram(prog_);
    vbo_ = vao_ = 0;
    prog_       = 0;
}

void TextOverlay::draw(std::string_view text,
                       float x,
                       float y,
                       float px,
                       int fb_w,
                       int fb_h,
                       float r,
                       float g,
                       float b) {
    verts_.clear();
    float cx = x;
    for (char ch : text) {
        std::uint8_t const* rows = glyph_for(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if (rows[row] & (1 << (4 - col))) {
                    float x0 = cx + col * px, y0 = y + row * px;
                    float x1 = x0 + px, y1 = y0 + px;
                    float quad[12] = {x0, y0, x1, y0, x1, y1, x0, y0, x1, y1, x0, y1};
                    verts_.insert(verts_.end(), quad, quad + 12);
                }
            }
        }
        cx += 6.0f * px; // 5 columns + 1 column of spacing
    }
    if (verts_.empty())
        return;

    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 verts_.size() * sizeof(float),
                 verts_.data(),
                 GL_STREAM_DRAW);
    glUniform2f(u_fb_, float(fb_w), float(fb_h));
    glUniform3f(u_color_, r, g, b);
    glDisable(GL_BLEND); // opaque text, readable over the additive scene
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(verts_.size() / 2));
    glEnable(GL_BLEND);
    glBindVertexArray(0);
}

// GL readback and BMP are both bottom-up, so rows are written as-is.
void write_bmp(char const* path, int w, int h, unsigned char const* rgba) {
    int const rowRaw = w * 3;
    int const pad    = (4 - (rowRaw % 4)) % 4;
    int const rowSz  = rowRaw + pad;
    int const imgSz  = rowSz * h;

    int const fileSz      = 54 + imgSz;
    unsigned char hdr[54] = {0};

    hdr[0]  = 'B';
    hdr[1]  = 'M';
    hdr[2]  = fileSz;
    hdr[3]  = fileSz >> 8;
    hdr[4]  = fileSz >> 16;
    hdr[5]  = fileSz >> 24;
    hdr[10] = 54; // pixel-data offset
    hdr[14] = 40; // DIB header size
    hdr[18] = w;
    hdr[19] = w >> 8;
    hdr[20] = w >> 16;
    hdr[21] = w >> 24;
    hdr[22] = h;
    hdr[23] = h >> 8;
    hdr[24] = h >> 16;
    hdr[25] = h >> 24;
    hdr[26] = 1;  // planes
    hdr[28] = 24; // bits per pixel
    hdr[34] = imgSz;
    hdr[35] = imgSz >> 8;
    hdr[36] = imgSz >> 16;
    hdr[37] = imgSz >> 24;

    std::FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row(rowSz, 0);
    for (int y = 0; y < h; ++y) {
        unsigned char const* src = rgba + std::size_t(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2]; // B
            row[x * 3 + 1] = src[x * 4 + 1]; // G
            row[x * 3 + 2] = src[x * 4 + 0]; // R
        }
        std::fwrite(row.data(), 1, rowSz, f);
    }
    std::fclose(f);
}

void key_cb(GLFWwindow* win, int key, int, int action, int) {
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(win, GLFW_TRUE);
}

} // namespace glutil
