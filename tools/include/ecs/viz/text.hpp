// ecs/viz/text.hpp
//
// Generic text/number helpers shared by both back-ends: XML escaping, a
// glyph-count width estimate (no font engine is available), and a compact
// number formatter. No ECS dependencies.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ecs::viz::detail {

// Run-budget badge for a system header: empty when it never retires.
inline std::string times_badge(std::uint64_t const times) {
    if (times == 0)
        return {};
    return times == 1 ? std::string {"(once)"} : "(x" + std::to_string(times) + ")";
}

inline std::string esc(std::string_view s) {
    std::string o;
    for (char c : s)
        switch (c) {
        case '&':
            o += "&amp;";
            break;
        case '<':
            o += "&lt;";
            break;
        case '>':
            o += "&gt;";
            break;
        default:
            o += c;
        }
    return o;
}

// --- text metrics (no font engine: estimate from glyph count) ----------------
inline std::size_t glyphs(std::string_view s) { // UTF-8 code points
    std::size_t c = 0;
    for (unsigned char ch : s)
        if ((ch & 0xC0) != 0x80)
            ++c;
    return c;
}
inline double approx(std::string_view s, double fs, bool bold = false) {
    return glyphs(s) * fs * (bold ? 0.63 : 0.56); // slight over-estimate
}
inline std::string num(double v) { return std::to_string(std::lround(v)); }

} // namespace ecs::viz::detail
