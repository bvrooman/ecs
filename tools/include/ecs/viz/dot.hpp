// ecs/viz/dot.hpp
//
// Graphviz-DOT back-end. dot_label() renders one NodeView as an HTML-like table
// label; viz::to_dot() assembles the phase/level-clustered digraph (pipe through
// `dot -Tsvg`). Graphviz does its own crossing minimisation, so -- unlike the
// SVG back-end -- there is no barycenter pass here.

#pragma once

#include <ecs/schedule.hpp>

#include "graph.hpp"
#include "name_table.hpp"
#include "node_view.hpp"
#include "text.hpp"
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ecs::viz::detail {

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
            std::string nm  = ln.resource ? "<i>" + esc(ln.label) +
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
    std::string header = "<tr><td colspan=\"2\" align=\"center\" bgcolor=\"" +
                         std::string(head) + "\"><font point-size=\"13\">" + hdr +
                         "</font></td></tr>";
    return "<table border=\"1\" cellborder=\"0\" cellspacing=\"0\" "
           "cellpadding=\"4\" bgcolor=\"" +
           std::string(body) + "\" color=\"#b9c2cf\">" + header + rows + "</table>";
}

} // namespace ecs::viz::detail

namespace ecs::viz {

inline std::string to_dot(ecs::Schedule& sched, NameTable const& names = {}) {
    static_cast<void>(sched.level_count()); // force (phase, level) assignment
    auto const& systems = sched.systems();
    // Tombstoned systems keep their slot but are not drawn.
    std::map<int, std::map<std::size_t, std::vector<ecs::SystemId>>> tree;
    for (auto&& [i, system] : systems.enumerate())
        if (!system.dead)
            tree[system.phase][system.level].push_back(i);

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
            out("    subgraph cluster_phase" + ptag + "_lvl" + std::to_string(level) +
                " {");
            out("      label=\"level " + std::to_string(level) + "\"; labeljust=l;");
            out("      style=\"rounded,dashed\"; color=\"#b9b9b9\"; "
                "fontcolor=\"#9a9a9a\"; fontsize=11;");
            out("      { rank=same;");
            for (auto i : idxs)
                out("        s" + std::to_string(i.value) + " [label=<" +
                    detail::dot_label(detail::node_view(systems[i], names)) + ">];");
            out("      }");
            out("    }");
        }
        out("  }");
    }
    for (auto const& [a, b] : detail::reduced_dependencies(sched))
        out("  s" + std::to_string(a) + " -> s" + std::to_string(b) + ";");
    // Invisible chain so phases stack in order even with no edge between them.
    std::optional<ecs::SystemId> prev;
    for (auto const& [phase, levels] : tree) {
        auto const first = levels.begin()->second.front();
        if (prev)
            out("  s" + std::to_string((*prev).value) + " -> s" +
                std::to_string(first.value) + " [style=invis];");
        prev = levels.rbegin()->second.front();
    }
    out("}");
    return dot;
}

} // namespace ecs::viz
