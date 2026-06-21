// ecs/schedule_dot.hpp
//
// Visualizers for a Schedule's wave DAG. Include this header to use
// Schedule::to_dot() / Schedule::to_svg():
//
//   #include <ecs/schedule_dot.hpp>
//   ecs::viz::NameTable nt;
//   nt.component<Position>("Position").resource<Gravity>("Gravity");
//   std::cout << sched.to_svg(nt);            // standalone SVG, no Graphviz
//   std::cout << sched.to_dot(nt);            // Graphviz DOT: | dot -Tsvg
//
// Both back-ends share the analysis (the scheduler's own conflict() via
// Schedule::reduced_dependencies_(), and a NodeView per system) -- only the
// rendering differs. Layout: an outer box per phase, an inner box per conflict
// level (a row of conflict-free systems), one box per system listing its full
// access signature (every component/resource with R/W, plus Commands). Edges
// are transitively reduced.
//
// The SVG back-end does its own layered layout: the scheduler already assigns
// each system a (phase, level), which is the hard part (rank assignment), so we
// only size boxes, place each level's row, and route edges as curves.

#pragma once

#include "entity.hpp"   // component_id
#include "resource.hpp" // resource_id
#include "schedule.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ecs::viz {

// Optional id->label map. Component and resource ids live in *separate* id
// spaces, so they get separate tables. Unknown ids fall back to "c<id>"/"r<id>".
struct NameTable {
    std::unordered_map<std::uint32_t, std::string> comp, res;

    template <class C>
    NameTable& component(std::string n) {
        comp[ecs::component_id<C>] = std::move(n);
        return *this;
    }
    template <class R>
    NameTable& resource(std::string n) {
        res[ecs::resource_id<R>] = std::move(n);
        return *this;
    }
    std::string comp_name(std::uint32_t id) const {
        auto it = comp.find(id);
        return it != comp.end() ? it->second : "c" + std::to_string(id);
    }
    std::string res_name(std::uint32_t id) const {
        auto it = res.find(id);
        return it != res.end() ? it->second : "r" + std::to_string(id);
    }
};

namespace detail {

    // --- shared content model ------------------------------------------------
    enum class Kind { Normal, ReadsAll, Exclusive, Structural };

    struct Line {
        enum Style { Access, Note, Commands } style = Access;
        std::string label;
        char tag      = 0;     // 'R' or 'W' for Access lines
        bool resource = false; // Access line for a resource (italic + "res")
    };

    struct NodeView {
        std::string name;
        bool once = false;
        Kind kind = Kind::Normal;
        std::vector<Line> lines;
    };

    inline NodeView node_view(Schedule::System const& s, NameTable const& nt) {
        auto const& a = s.access;
        NodeView v;
        v.name = s.name;
        v.once = s.once;
        bool const has = !a.reads.empty() || !a.writes.empty() ||
                         !a.res_reads.empty() || !a.res_writes.empty();
        v.kind = a.exclusive  ? Kind::Exclusive
                 : a.reads_all ? Kind::ReadsAll
                 : has         ? Kind::Normal
                               : Kind::Structural;
        for (auto id : a.writes)
            v.lines.push_back({Line::Access, nt.comp_name(id), 'W', false});
        for (auto id : a.reads)
            v.lines.push_back({Line::Access, nt.comp_name(id), 'R', false});
        for (auto id : a.res_writes)
            v.lines.push_back({Line::Access, nt.res_name(id), 'W', true});
        for (auto id : a.res_reads)
            v.lines.push_back({Line::Access, nt.res_name(id), 'R', true});
        if (a.reads_all)
            v.lines.push_back({Line::Note, "reads everything", 0, false});
        if (a.exclusive)
            v.lines.push_back({Line::Note, "World& — exclusive", 0, false});
        if (a.commands)
            v.lines.push_back({Line::Commands, "Commands", 0, false});
        if (v.lines.empty())
            v.lines.push_back({Line::Note, "(no access)", 0, false});
        return v;
    }

    // {body, header} fills per kind.
    inline std::pair<char const*, char const*> palette(Kind k) {
        switch (k) {
        case Kind::Exclusive: return {"#fbe6e6", "#f1c6c6"}; // runs alone
        case Kind::ReadsAll:  return {"#eaf7ea", "#c8e6c8"}; // reads everything
        case Kind::Normal:    return {"#eef3fb", "#c9ddf6"};
        default:              return {"#f1efe8", "#dad7ce"}; // structural / none
        }
    }

    inline constexpr char kRead[]  = "#1f7a4d"; // green
    inline constexpr char kWrite[] = "#c0561f"; // orange

    inline std::string esc(std::string_view s) {
        std::string o;
        for (char c : s)
            switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            default: o += c;
            }
        return o;
    }

    // --- text metrics (no font engine: estimate from glyph count) ------------
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

    // --- DOT (HTML-like label) ----------------------------------------------
    inline std::string dot_label(NodeView const& v) {
        auto [body, head] = palette(v.kind);
        std::string hdr   = "<b>" + esc(v.name) + "</b>";
        if (v.once)
            hdr += "<font color=\"#888888\" point-size=\"10\">  (once)</font>";
        std::string rows;
        for (auto const& ln : v.lines) {
            if (ln.style == Line::Note)
                rows += "<tr><td colspan=\"2\" align=\"left\"><font "
                        "point-size=\"11\"><i>" +
                        esc(ln.label) + "</i></font></td></tr>";
            else if (ln.style == Line::Commands)
                rows += "<tr><td colspan=\"2\" align=\"left\"><font "
                        "color=\"#7a6f5a\" point-size=\"11\">" +
                        esc(ln.label) + "</font></td></tr>";
            else {
                std::string nm =
                    ln.resource ? "<i>" + esc(ln.label) +
                                      "</i> <font color=\"#9a9a9a\" "
                                      "point-size=\"8\">res</font>"
                                : esc(ln.label);
                char const* col = ln.tag == 'W' ? kWrite : kRead;
                rows += "<tr><td align=\"left\"><font point-size=\"11\">" + nm +
                        "</font></td><td align=\"right\"><font color=\"" + col +
                        "\" point-size=\"11\"><b>" + std::string(1, ln.tag) +
                        "</b></font></td></tr>";
            }
        }
        std::string header =
            "<tr><td colspan=\"2\" align=\"center\" bgcolor=\"" +
            std::string(head) + "\"><font point-size=\"13\">" + hdr +
            "</font></td></tr>";
        return "<table border=\"1\" cellborder=\"0\" cellspacing=\"0\" "
               "cellpadding=\"4\" bgcolor=\"" +
               std::string(body) + "\" color=\"#b9c2cf\">" + header + rows +
               "</table>";
    }

    // --- SVG box sizing + rendering ------------------------------------------
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

    inline std::string svg_text(double x, double y, char const* anchor,
                                double fs, char const* fill, std::string body,
                                bool italic = false, int weight = 400) {
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

    inline std::string svg_node(NodeView const& v, double x, double y, double w,
                                double h) {
        auto [body, head] = palette(v.kind);
        std::string const X = num(x), Y = num(y), W = num(w), H = num(h);
        std::string s;
        // body fill, rounded-top header, divider, outer border
        s += "<rect x=\"" + X + "\" y=\"" + Y + "\" width=\"" + W +
             "\" height=\"" + H + "\" rx=\"6\" fill=\"" + body + "\"/>";
        s += "<path d=\"M" + num(x + 6) + " " + Y + " H" + num(x + w - 6) +
             " A6 6 0 0 1 " + num(x + w) + " " + num(y + 6) + " V" + num(y + 24) +
             " H" + X + " V" + num(y + 6) + " A6 6 0 0 1 " + num(x + 6) + " " + Y +
             " Z\" fill=\"" + head + "\"/>";
        s += "<line x1=\"" + X + "\" y1=\"" + num(y + 24) + "\" x2=\"" +
             num(x + w) + "\" y2=\"" + num(y + 24) +
             "\" stroke=\"#b9c2cf\" stroke-width=\"0.5\"/>";
        s += "<rect x=\"" + X + "\" y=\"" + Y + "\" width=\"" + W +
             "\" height=\"" + H +
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
                s += svg_text(x + w / 2, base, "middle", 11, "#3a4452",
                              esc(ln.label), /*italic=*/true);
            else if (ln.style == Line::Commands)
                s += svg_text(x + kPadX, base, nullptr, 11, "#7a6f5a",
                              esc(ln.label));
            else {
                s += svg_text(x + kPadX, base, nullptr, 11, "#3a4452",
                              esc(ln.label), /*italic=*/ln.resource);
                if (ln.resource)
                    s += svg_text(x + kPadX + approx(ln.label, 11) + 4, base,
                                  nullptr, 8, "#9a9a9a", "res");
                s += svg_text(x + w - kPadX, base, "end", 11,
                              ln.tag == 'W' ? kWrite : kRead,
                              std::string(1, ln.tag), false, 500);
            }
            base += kLineH;
        }
        return s;
    }

} // namespace detail
} // namespace ecs::viz

namespace ecs {

inline std::vector<std::pair<std::size_t, std::size_t>>
Schedule::reduced_dependencies_() const {
    auto const n = systems_.size();
    // Direct conflict edges j -> i (j < i, same phase) -- the scheduler's own
    // predicate. Registration order sets the direction.
    std::vector<std::vector<std::size_t>> succ(n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (systems_[j].phase == systems_[i].phase &&
                conflict(systems_[j], systems_[i]))
                succ[j].push_back(i);

    // Reachability: edges only point to higher indices, so one high->low pass
    // suffices. Then keep a->b only if no other successor of a already reaches b.
    std::vector<std::vector<char>> reach(n, std::vector<char>(n, 0));
    for (std::size_t a = n; a-- > 0;)
        for (auto b : succ[a]) {
            reach[a][b] = 1;
            for (std::size_t k = 0; k < n; ++k)
                reach[a][k] = static_cast<char>(reach[a][k] || reach[b][k]);
        }
    std::vector<std::pair<std::size_t, std::size_t>> edges;
    for (std::size_t a = 0; a < n; ++a)
        for (auto b : succ[a]) {
            bool essential = true;
            for (auto c : succ[a])
                if (c != b && reach[c][b]) {
                    essential = false;
                    break;
                }
            if (essential)
                edges.emplace_back(a, b);
        }
    return edges;
}

inline std::string Schedule::to_dot(viz::NameTable const& names) {
    rebuild();
    auto const n = systems_.size();
    std::map<int, std::map<std::size_t, std::vector<std::size_t>>> tree;
    for (std::size_t i = 0; i < n; ++i)
        tree[systems_[i].phase][systems_[i].level].push_back(i);

    std::string dot;
    auto out = [&](std::string s) { dot += std::move(s) + "\n"; };
    out("digraph schedule {");
    out("  compound=true; rankdir=TB;");
    out("  graph [fontname=Helvetica];");
    out("  node  [shape=plaintext fontname=Helvetica];");
    out("  edge  [color=\"#b3b3b3\" arrowsize=0.7];");
    for (auto const& [phase, levels] : tree) {
        std::string ptag = (phase < 0 ? "n" : "") + std::to_string(std::abs(phase));
        out("  subgraph cluster_phase" + ptag + " {");
        out("    label=\"phase " + std::to_string(phase) + "\"; labeljust=l;");
        out("    style=\"rounded,filled\"; color=\"#cfcfcf\"; "
            "fillcolor=\"#fafafa\"; fontcolor=\"#6b6b6b\"; fontsize=14;");
        for (auto const& [level, idxs] : levels) {
            out("    subgraph cluster_phase" + ptag + "_lvl" +
                std::to_string(level) + " {");
            out("      label=\"level " + std::to_string(level) + "\"; labeljust=l;");
            out("      style=\"rounded,dashed\"; color=\"#b9b9b9\"; "
                "fontcolor=\"#9a9a9a\"; fontsize=11;");
            out("      { rank=same;");
            for (auto i : idxs)
                out("        s" + std::to_string(i) + " [label=<" +
                    viz::detail::dot_label(viz::detail::node_view(systems_[i], names)) +
                    ">];");
            out("      }");
            out("    }");
        }
        out("  }");
    }
    for (auto const& [a, b] : reduced_dependencies_())
        out("  s" + std::to_string(a) + " -> s" + std::to_string(b) + ";");
    // Invisible chain so phases stack in order even with no edge between them.
    std::optional<std::size_t> prev;
    for (auto const& [phase, levels] : tree) {
        std::size_t const first = levels.begin()->second.front();
        if (prev)
            out("  s" + std::to_string(*prev) + " -> s" + std::to_string(first) +
                " [style=invis];");
        prev = levels.rbegin()->second.front();
    }
    out("}");
    return dot;
}

inline std::string Schedule::to_svg(viz::NameTable const& names) {
    using namespace viz::detail;
    rebuild();
    auto const n = systems_.size();

    // Per-system content + sizes.
    std::vector<NodeView> nv(n);
    std::vector<double> bw(n), bh(n);
    for (std::size_t i = 0; i < n; ++i) {
        nv[i] = node_view(systems_[i], names);
        bw[i] = box_w(nv[i]);
        bh[i] = box_h(nv[i]);
    }

    // phase -> level -> indices (ascending = the executor's wave order).
    std::map<int, std::map<std::size_t, std::vector<std::size_t>>> tree;
    for (std::size_t i = 0; i < n; ++i)
        tree[systems_[i].phase][systems_[i].level].push_back(i);

    // Content width = widest level row; every phase/level shares it so columns
    // line up and chain edges stay vertical.
    constexpr double gap = 28, lane = 30, phasePad = 12, leftM = 16, topM = 16;
    double contentW = 120;
    for (auto const& [phase, levels] : tree)
        for (auto const& [level, idxs] : levels) {
            double row = -gap;
            for (auto i : idxs)
                row += bw[i] + gap;
            contentW = std::max(contentW, row);
        }
    double const contentLeft = leftM + phasePad + lane;
    double const centerX     = contentLeft + contentW / 2;
    double const phaseBoxW   = contentW + 2 * lane + 2 * phasePad;

    std::vector<double> cx(n), topY(n), botY(n);
    std::vector<std::size_t> lvl(n);
    std::string boxes, nodes;

    double y = topM;
    for (auto const& [phase, levels] : tree) {
        double const phaseTop = y;
        double yy             = y + 22; // phase label band
        std::string levelStr;
        for (auto const& [level, idxs] : levels) {
            double const levelTop = yy;
            double rowMaxH = 0, rowW = -gap;
            for (auto i : idxs) {
                rowMaxH = std::max(rowMaxH, bh[i]);
                rowW += bw[i] + gap;
            }
            double const rowTop = levelTop + 16 + 8; // level label + pad
            double rx           = contentLeft + (contentW - rowW) / 2;
            for (auto i : idxs) {
                nodes += svg_node(nv[i], rx, rowTop, bw[i], bh[i]);
                cx[i]   = rx + bw[i] / 2;
                topY[i] = rowTop;
                botY[i] = rowTop + bh[i];
                lvl[i]  = level;
                rx += bw[i] + gap;
            }
            double const levelBot = rowTop + rowMaxH + 8;
            levelStr += "<rect x=\"" + num(contentLeft) + "\" y=\"" +
                        num(levelTop) + "\" width=\"" + num(contentW) +
                        "\" height=\"" + num(levelBot - levelTop) +
                        "\" rx=\"10\" fill=\"none\" stroke=\"#b9b9b9\" "
                        "stroke-width=\"0.7\" stroke-dasharray=\"4 4\"/>";
            levelStr += svg_text(contentLeft + 10, levelTop + 13, nullptr, 11,
                                 "#9a9a9a", "level " + std::to_string(level));
            yy = levelBot + 14;
        }
        double const phaseBot = yy - 14 + 10;
        boxes += "<rect x=\"" + num(leftM) + "\" y=\"" + num(phaseTop) +
                 "\" width=\"" + num(phaseBoxW) + "\" height=\"" +
                 num(phaseBot - phaseTop) +
                 "\" rx=\"14\" fill=\"#fafafa\" stroke=\"#cfcfcf\" "
                 "stroke-width=\"0.8\"/>";
        boxes += svg_text(leftM + 14, phaseTop + 16, nullptr, 13, "#6b6b6b",
                          "phase " + std::to_string(phase));
        boxes += levelStr;
        y = phaseBot + 18;
    }
    double const totalH = y - 18 + topM;
    double const totalW = leftM * 2 + phaseBoxW;

    // Edges (transitively reduced). Adjacent levels: near-vertical curve.
    // Level-skipping edges bow out into a side lane to clear intervening boxes.
    std::string edges;
    double const leftLane = leftM + phasePad + lane * 0.5;
    double const rightLane = contentLeft + contentW + lane * 0.5;
    for (auto const& [a, b] : reduced_dependencies_()) {
        double sx = cx[a], sy = botY[a], tx = cx[b], ty = topY[b], dy = ty - sy;
        std::string d;
        if (lvl[b] == lvl[a] + 1)
            d = "M" + num(sx) + " " + num(sy) + " C" + num(sx) + " " +
                num(sy + dy * 0.45) + " " + num(tx) + " " + num(ty - dy * 0.45) +
                " " + num(tx) + " " + num(ty);
        else {
            double bowX = (sx <= centerX ? leftLane : rightLane);
            d = "M" + num(sx) + " " + num(sy) + " C" + num(bowX) + " " +
                num(sy + dy * 0.3) + " " + num(bowX) + " " + num(ty - dy * 0.3) +
                " " + num(tx) + " " + num(ty);
        }
        edges += "<path d=\"" + d +
                 "\" fill=\"none\" stroke=\"#b0b0b0\" stroke-width=\"1\" "
                 "marker-end=\"url(#ah)\"/>";
    }

    std::string svg;
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(totalW) +
           "\" height=\"" + num(totalH) + "\" viewBox=\"0 0 " + num(totalW) +
           " " + num(totalH) +
           "\" font-family=\"Helvetica,Arial,sans-serif\">";
    svg += "<defs><marker id=\"ah\" viewBox=\"0 0 10 10\" refX=\"8\" refY=\"5\" "
           "markerWidth=\"6\" markerHeight=\"6\" orient=\"auto-start-reverse\">"
           "<path d=\"M2 1 L8 5 L2 9\" fill=\"none\" stroke=\"context-stroke\" "
           "stroke-width=\"1.5\" stroke-linecap=\"round\" "
           "stroke-linejoin=\"round\"/></marker></defs>";
    svg += boxes + edges + nodes + "</svg>";
    return svg;
}

inline std::string Schedule::to_dot() { return to_dot(viz::NameTable {}); }
inline std::string Schedule::to_svg() { return to_svg(viz::NameTable {}); }

} // namespace ecs
