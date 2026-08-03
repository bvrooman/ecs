// ecs/viz/node_view.hpp
//
// Backend-neutral content model for a system box. A NodeView captures *what* to
// draw -- the box kind plus a list of access lines -- independent of DOT vs SVG;
// node_view() derives it from a system's declared SystemAccess. Both back-ends
// consume this, so the access->rows mapping lives in exactly one place.

#pragma once

#include <ecs/schedule.hpp>
#include <ecs/viz/name_table.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ecs::viz::detail {

enum class Kind { Normal, ReadsAll, Exclusive, Structural };

struct Line {
    enum Style { Access, Note, Commands } style = Access;
    std::string label;
    char tag      = 0;     // 'R' or 'W' for Access lines
    bool resource = false; // Access line for a resource (italic + "res")
};

struct NodeView {
    std::string name;
    // Run budget from SystemRecord::times (0 = never retires). Rendered as a
    // badge: "(once)" at 1, "(xN)" above that.
    std::uint64_t times = 0;
    Kind kind           = Kind::Normal;
    std::vector<Line> lines;
};

inline NodeView node_view(ecs::Schedule::System const& s, NameTable const& nt) {
    auto const& a = s.access;
    NodeView v;
    v.name         = s.name;
    v.times        = s.times;
    bool const has = !a.reads.empty() || !a.writes.empty() || !a.res_reads.empty() ||
                     !a.res_writes.empty() || !a.res_folds.empty();
    v.kind         = a.exclusive   ? Kind::Exclusive
                     : a.reads_all ? Kind::ReadsAll
                     : has         ? Kind::Normal
                                   : Kind::Structural;
    for (auto id : a.writes)
        v.lines.push_back({Line::Access, nt.comp_name(id), 'W', false});
    for (auto id : a.reads)
        v.lines.push_back({Line::Access, nt.comp_name(id), 'R', false});
    for (auto id : a.res_writes)
        v.lines.push_back({Line::Access, nt.res_name(id), 'W', true});
    // Folds render as writes: that is their relation to everything the graph
    // draws an edge for. Fold-vs-fold shows up as the absent edge.
    for (auto id : a.res_folds)
        v.lines.push_back({Line::Access, nt.res_name(id), 'W', true});
    for (auto id : a.res_reads)
        v.lines.push_back({Line::Access, nt.res_name(id), 'R', true});
    if (a.reads_all)
        v.lines.push_back({Line::Note, "reads everything", 0, false});
    if (a.exclusive)
        v.lines.push_back({Line::Note, "exclusive — unanalyzable access", 0, false});
    if (a.commands)
        v.lines.push_back({Line::Commands, "Commands", 0, false});
    if (v.lines.empty())
        v.lines.push_back({Line::Note, "(no access)", 0, false});
    return v;
}

// {body, header} fills per kind.
inline std::pair<char const*, char const*> palette(Kind k) {
    switch (k) {
    case Kind::Exclusive:
        return {"#fbe6e6", "#f1c6c6"}; // runs alone
    case Kind::ReadsAll:
        return {"#eaf7ea", "#c8e6c8"}; // reads everything
    case Kind::Normal:
        return {"#eef3fb", "#c9ddf6"};
    default:
        return {"#f1efe8", "#dad7ce"}; // structural / none
    }
}

inline constexpr char kRead[]  = "#1f7a4d"; // green
inline constexpr char kWrite[] = "#c0561f"; // orange

} // namespace ecs::viz::detail
