// tools/schedule_dot.hpp
//
// Standalone visualizer for an ecs::Schedule's wave DAG -- free functions built
// entirely on the *public* Schedule API: systems(), level_count(), and the
// Schedule::conflicts() predicate. Nothing here re-derives the scheduler's
// conflict analysis, and the core scheduler carries no visualization code.
//
//   #include "schedule_dot.hpp"
//   viz::NameTable nt;
//   nt.component<Position>("Position").resource<Gravity>("Gravity");
//   std::cout << viz::to_svg(sched, nt);   // standalone SVG, no Graphviz
//   std::cout << viz::to_dot(sched, nt);   // Graphviz DOT: | dot -Tsvg
//
// Both back-ends share the analysis (reduced_dependencies() + a NodeView per
// system) -- only rendering differs. Layout: an outer box per phase, an inner
// box per conflict level (a row of conflict-free systems), one box per system
// listing its full access signature (every component/resource with R/W, plus
// Commands). Edges are transitively reduced; the SVG back-end also runs a
// barycenter pass to cut crossings.
//
// The SVG layout leans on the scheduler having already assigned each system a
// (phase, level) -- the hard part (rank assignment) -- so we only size boxes,
// place each level's row, and route edges as curves.

#pragma once

#include "ecs/entity.hpp"   // component_id
#include "ecs/resource.hpp" // resource_id
#include "ecs/schedule.hpp"

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

namespace viz {

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

    inline NodeView node_view(ecs::Schedule::System const& s, NameTable const& nt) {
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

    // --- dependency analysis (uses the scheduler's own predicate) ------------
    // Transitively-reduced dependency edges (a -> b, a < b within a phase) built
    // from Schedule::conflicts -- the same predicate the wavefront leveling uses.
    inline std::vector<std::pair<std::size_t, std::size_t>>
    reduced_dependencies(ecs::Schedule const& sched) {
        auto const& sys = sched.systems();
        auto const n    = sys.size();
        // Direct conflict edges j -> i (j < i, same phase). Registration order
        // sets the direction.
        std::vector<std::vector<std::size_t>> succ(n);
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < i; ++j)
                if (sys[j].phase == sys[i].phase &&
                    ecs::Schedule::conflicts(sys[j].access, sys[i].access))
                    succ[j].push_back(i);

        // Reachability: edges only point to higher indices, so one high->low pass
        // suffices. Keep a->b only if no other successor of a already reaches b.
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

    // --- crossing reduction (barycenter ordering) ---------------------------
    // Edge crossings between consecutive layers for the current node ordering.
    inline std::size_t layer_crossings(
        std::vector<std::vector<std::size_t>> const& layers,
        std::vector<std::vector<std::size_t>> const& nbrDown,
        std::unordered_map<std::size_t, std::size_t> const& pos) {
        std::size_t total = 0;
        for (std::size_t k = 0; k + 1 < layers.size(); ++k) {
            std::vector<std::pair<std::size_t, std::size_t>> e; // (upperPos, lowerPos)
            for (auto a : layers[k])
                for (auto b : nbrDown[a])
                    e.push_back({pos.at(a), pos.at(b)});
            for (std::size_t i = 0; i < e.size(); ++i)
                for (std::size_t j = i + 1; j < e.size(); ++j)
                    if (e[i].first != e[j].first && e[i].second != e[j].second &&
                        (e[i].first < e[j].first) != (e[i].second < e[j].second))
                        ++total;
        }
        return total;
    }

    // Reorder systems within each level to cut crossings of the adjacent-level
    // edges, via up/down barycenter sweeps (the classic Sugiyama heuristic).
    // Ties keep the prior order, which starts as registration order, so the
    // result is deterministic. Level-skipping edges are routed in side lanes and
    // play no part here. layers[k] holds the nodes at level k of one phase.
    inline void order_phase(
        std::vector<std::vector<std::size_t>>& layers,
        std::vector<std::vector<std::size_t>> const& nbrUp,
        std::vector<std::vector<std::size_t>> const& nbrDown) {
        if (layers.size() < 2)
            return;
        std::unordered_map<std::size_t, std::size_t> pos;
        auto repos = [&] {
            pos.clear();
            for (auto const& l : layers)
                for (std::size_t i = 0; i < l.size(); ++i)
                    pos[l[i]] = i;
        };
        repos();

        // Sort one layer by the mean position of each node's neighbours in the
        // reference layer (above for a down-sweep, below for an up-sweep). Nodes
        // with no such neighbour keep their slot (current index, scaled into the
        // reference's coordinate range so the comparison is meaningful).
        auto sort_layer = [&](std::size_t k, bool use_up) {
            auto& lay = layers[k];
            if (lay.size() < 2)
                return;
            std::size_t const ref = use_up ? layers[k - 1].size() : layers[k + 1].size();
            std::vector<std::pair<double, std::size_t>> keyed;
            keyed.reserve(lay.size());
            for (std::size_t i = 0; i < lay.size(); ++i) {
                std::size_t const v = lay[i];
                auto const& nb      = use_up ? nbrUp[v] : nbrDown[v];
                double key;
                if (nb.empty())
                    key = (ref <= 1 || lay.size() <= 1)
                              ? double(i)
                              : double(i) * double(ref - 1) / double(lay.size() - 1);
                else {
                    double s = 0;
                    for (auto u : nb)
                        s += double(pos[u]);
                    key = s / double(nb.size());
                }
                keyed.push_back({key, v});
            }
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](auto const& a, auto const& b) { return a.first < b.first; });
            for (std::size_t i = 0; i < lay.size(); ++i) {
                lay[i]      = keyed[i].second;
                pos[lay[i]] = i;
            }
        };

        auto best          = layers;
        std::size_t best_c = layer_crossings(layers, nbrDown, pos);
        for (int iter = 0; iter < 8 && best_c > 0; ++iter) {
            for (std::size_t k = 1; k < layers.size(); ++k)
                sort_layer(k, /*use_up=*/true); // down sweep: order by layer above
            for (std::size_t k = layers.size() - 1; k-- > 0;)
                sort_layer(k, /*use_up=*/false); // up sweep: order by layer below
            repos();
            std::size_t const c = layer_crossings(layers, nbrDown, pos);
            if (c < best_c) {
                best_c = c;
                best   = layers;
            }
        }
        layers = best;
    }

} // namespace detail

inline std::string to_dot(ecs::Schedule& sched, NameTable const& names = {}) {
    static_cast<void>(sched.level_count()); // force (phase, level) assignment
    auto const& sys = sched.systems();
    auto const n    = sys.size();
    std::map<int, std::map<std::size_t, std::vector<std::size_t>>> tree;
    for (std::size_t i = 0; i < n; ++i)
        tree[sys[i].phase][sys[i].level].push_back(i);

    std::string dot;
    auto out = [&](std::string str) { dot += std::move(str) + "\n"; };
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
                    detail::dot_label(detail::node_view(sys[i], names)) + ">];");
            out("      }");
            out("    }");
        }
        out("  }");
    }
    for (auto const& [a, b] : detail::reduced_dependencies(sched))
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

inline std::string to_svg(ecs::Schedule& sched, NameTable const& names = {}) {
    using namespace detail;
    static_cast<void>(sched.level_count()); // force (phase, level) assignment
    auto const& sys = sched.systems();
    auto const n    = sys.size();

    // Per-system content + sizes.
    std::vector<NodeView> nv(n);
    std::vector<double> bw(n), bh(n);
    for (std::size_t i = 0; i < n; ++i) {
        nv[i] = node_view(sys[i], names);
        bw[i] = box_w(nv[i]);
        bh[i] = box_h(nv[i]);
    }

    // phase -> level -> indices (ascending = the executor's wave order).
    std::map<int, std::map<std::size_t, std::vector<std::size_t>>> tree;
    for (std::size_t i = 0; i < n; ++i)
        tree[sys[i].phase][sys[i].level].push_back(i);

    // Reduce edge crossings: reorder systems within each level (barycenter
    // sweeps over the drawn edges). Same-level systems are conflict-free, so
    // their left/right order is free to change; registration order is the
    // start and tie-break, so the layout stays deterministic.
    auto const edges = reduced_dependencies(sched);
    std::vector<std::vector<std::size_t>> nbrUp(n), nbrDown(n);
    for (auto const& [a, b] : edges)
        if (sys[b].level == sys[a].level + 1) { // adjacent levels only
            nbrDown[a].push_back(b);
            nbrUp[b].push_back(a);
        }
    for (auto& [phase, levels] : tree) {
        std::vector<std::vector<std::size_t>> layers(levels.rbegin()->first + 1);
        for (auto const& [level, idxs] : levels)
            layers[level] = idxs;
        order_phase(layers, nbrUp, nbrDown);
        for (auto& [level, idxs] : levels)
            idxs = layers[level];
    }

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
    std::string edgeSvg;
    double const leftLane  = leftM + phasePad + lane * 0.5;
    double const rightLane = contentLeft + contentW + lane * 0.5;
    for (auto const& [a, b] : edges) {
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
        edgeSvg += "<path d=\"" + d +
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
    svg += boxes + edgeSvg + nodes + "</svg>";
    return svg;
}

} // namespace viz
