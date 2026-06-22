// tools/schedule_viz/svg_node.hpp
//
// SVG rendering of a single system box: size estimation (box_w/box_h from the
// NodeView) and the box itself (svg_node), plus a small svg_text helper. The
// to_svg() driver in svg.hpp positions these boxes; this file owns only how one
// box looks.

#pragma once

#include "node_view.hpp"
#include "text.hpp"
#include <algorithm>
#include <string>

namespace viz::detail {

inline constexpr double kHeaderH = 24, kLineH = 18, kBotPad = 6, kPadX = 12;

inline double box_w(NodeView const& v) {
    double w = approx(v.name, 13, true) + (v.once ? approx(" (once)", 10) : 0);
    for (auto const& ln : v.lines) {
        double lw = approx(ln.label, 11);
        if (ln.style == Line::Access)
            lw += (ln.resource ? approx(" res", 8) : 0) + 16 + 9; // gap + tag
        w = std::max(w, lw);
    }
    return std::max(120.0, w + 2 * kPadX);
}
inline double box_h(NodeView const& v) {
    return kHeaderH + double(v.lines.size()) * kLineH + kBotPad;
}

inline std::string svg_text(double x,
                            double y,
                            char const* anchor,
                            double fs,
                            char const* fill,
                            std::string body,
                            bool italic = false,
                            int weight  = 400) {
    std::string s = "<text x=\"" + num(x) + "\" y=\"" + num(y) + "\"";
    if (anchor)
        s += std::string(" text-anchor=\"") + anchor + "\"";
    s += " font-size=\"" + num(fs) + "\" fill=\"" + fill + "\"";
    if (italic)
        s += " font-style=\"italic\"";
    if (weight != 400)
        s += " font-weight=\"" + std::to_string(weight) + "\"";
    return s + ">" + std::move(body) + "</text>";
}

inline std::string svg_node(NodeView const& v, double x, double y, double w, double h) {
    auto [body, head]   = palette(v.kind);
    std::string const X = num(x), Y = num(y), W = num(w), H = num(h);
    std::string s;
    // body fill, rounded-top header, divider, outer border
    s += "<rect x=\"" + X + "\" y=\"" + Y + "\" width=\"" + W + "\" height=\"" + H +
         "\" rx=\"6\" fill=\"" + body + "\"/>";
    s += "<path d=\"M" + num(x + 6) + " " + Y + " H" + num(x + w - 6) + " A6 6 0 0 1 " +
         num(x + w) + " " + num(y + 6) + " V" + num(y + 24) + " H" + X + " V" +
         num(y + 6) + " A6 6 0 0 1 " + num(x + 6) + " " + Y + " Z\" fill=\"" + head +
         "\"/>";
    s += "<line x1=\"" + X + "\" y1=\"" + num(y + 24) + "\" x2=\"" + num(x + w) +
         "\" y2=\"" + num(y + 24) + "\" stroke=\"#b9c2cf\" stroke-width=\"0.5\"/>";
    s += "<rect x=\"" + X + "\" y=\"" + Y + "\" width=\"" + W + "\" height=\"" + H +
         "\" rx=\"6\" fill=\"none\" stroke=\"#b9c2cf\" stroke-width=\"0.6\"/>";
    // header
    std::string hd = "<tspan font-weight=\"500\">" + esc(v.name) + "</tspan>";
    if (v.once)
        hd += "<tspan font-size=\"10\" fill=\"#8a8a8a\">&#160;&#160;(once)</tspan>";
    s += svg_text(x + w / 2, y + 16, "middle", 13, "#2b3440", hd);
    // rows
    double base = y + 24 + 13;
    for (auto const& ln : v.lines) {
        if (ln.style == Line::Note)
            s += svg_text(x + w / 2,
                          base,
                          "middle",
                          11,
                          "#3a4452",
                          esc(ln.label),
                          /*italic=*/true);
        else if (ln.style == Line::Commands)
            s += svg_text(x + kPadX, base, nullptr, 11, "#7a6f5a", esc(ln.label));
        else {
            s += svg_text(x + kPadX,
                          base,
                          nullptr,
                          11,
                          "#3a4452",
                          esc(ln.label),
                          /*italic=*/ln.resource);
            if (ln.resource)
                s += svg_text(x + kPadX + approx(ln.label, 11) + 4,
                              base,
                              nullptr,
                              8,
                              "#9a9a9a",
                              "res");
            s += svg_text(x + w - kPadX,
                          base,
                          "end",
                          11,
                          ln.tag == 'W' ? kWrite : kRead,
                          std::string(1, ln.tag),
                          false,
                          500);
        }
        base += kLineH;
    }
    return s;
}

} // namespace viz::detail
