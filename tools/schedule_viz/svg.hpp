// tools/schedule_viz/svg.hpp
//
// Direct-SVG back-end: viz::to_svg() lays out the wave DAG with no Graphviz. The
// scheduler has already assigned each system a (phase, level) -- the hard part
// (rank assignment) -- so this only sizes boxes, places each level's row, runs a
// barycenter pass to cut crossings, and routes edges as curves (level-skipping
// edges bow out into a side lane to clear intervening boxes).

#pragma once

#include "ecs/schedule.hpp"
#include "graph.hpp"
#include "name_table.hpp"
#include "node_view.hpp"
#include "svg_node.hpp"
#include "text.hpp"
#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace viz {

inline std::string to_svg(ecs::Schedule& sched, NameTable const& names = {}) {
    using namespace detail;
    static_cast<void>(sched.level_count()); // force (phase, level) assignment
    auto const& systems = sched.systems();
    auto const n        = systems.size();

    // Per-system content + sizes.
    std::vector<NodeView> nv(n);
    std::vector<double> bw(n), bh(n);
    for (auto&& [id, system] : systems.enumerate()) {
        auto i = id.value;
        nv[i]  = node_view(system, names);
        bw[i]  = box_w(nv[i]);
        bh[i]  = box_h(nv[i]);
    }

    // phase -> level -> indices (ascending = the executor's wave order).
    // Tombstoned systems keep their slot but are not drawn.
    std::map<int, std::map<std::size_t, std::vector<std::size_t>>> tree;
    for (auto&& [id, system] : systems.enumerate())
        if (!system.dead)
            tree[system.phase][system.level].push_back(id.value);

    // Reduce edge crossings: reorder systems within each level (barycenter
    // sweeps over the drawn edges). Same-level systems are conflict-free, so
    // their left/right order is free to change; registration order is the
    // start and tie-break, so the layout stays deterministic.
    auto const edges = reduced_dependencies(sched);
    std::vector<std::vector<std::size_t>> nbrUp(n), nbrDown(n);
    for (auto const& [a, b] : edges) {
        if (systems[sid(b)].level == systems[sid(a)].level + 1) { // adjacent levels
            nbrDown[a].push_back(b);
            nbrUp[b].push_back(a);
        }
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
            levelStr += "<rect x=\"" + num(contentLeft) + "\" y=\"" + num(levelTop) +
                        "\" width=\"" + num(contentW) + "\" height=\"" +
                        num(levelBot - levelTop) +
                        "\" rx=\"10\" fill=\"none\" stroke=\"#b9b9b9\" "
                        "stroke-width=\"0.7\" stroke-dasharray=\"4 4\"/>";
            levelStr += svg_text(contentLeft + 10,
                                 levelTop + 13,
                                 nullptr,
                                 11,
                                 "#9a9a9a",
                                 "level " + std::to_string(level));
            yy = levelBot + 14;
        }
        double const phaseBot = yy - 14 + 10;
        boxes += "<rect x=\"" + num(leftM) + "\" y=\"" + num(phaseTop) + "\" width=\"" +
                 num(phaseBoxW) + "\" height=\"" + num(phaseBot - phaseTop) +
                 "\" rx=\"14\" fill=\"#fafafa\" stroke=\"#cfcfcf\" "
                 "stroke-width=\"0.8\"/>";
        boxes += svg_text(leftM + 14,
                          phaseTop + 16,
                          nullptr,
                          13,
                          "#6b6b6b",
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
                num(sy + dy * 0.45) + " " + num(tx) + " " + num(ty - dy * 0.45) + " " +
                num(tx) + " " + num(ty);
        else {
            double bowX = (sx <= centerX ? leftLane : rightLane);
            d           = "M" + num(sx) + " " + num(sy) + " C" + num(bowX) + " " +
                num(sy + dy * 0.3) + " " + num(bowX) + " " + num(ty - dy * 0.3) + " " +
                num(tx) + " " + num(ty);
        }
        edgeSvg += "<path d=\"" + d +
                   "\" fill=\"none\" stroke=\"#b0b0b0\" stroke-width=\"1\" "
                   "marker-end=\"url(#ah)\"/>";
    }

    std::string svg;
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + num(totalW) +
           "\" height=\"" + num(totalH) + "\" viewBox=\"0 0 " + num(totalW) + " " +
           num(totalH) + "\" font-family=\"Helvetica,Arial,sans-serif\">";
    svg += "<defs><marker id=\"ah\" viewBox=\"0 0 10 10\" refX=\"8\" refY=\"5\" "
           "markerWidth=\"6\" markerHeight=\"6\" orient=\"auto-start-reverse\">"
           "<path d=\"M2 1 L8 5 L2 9\" fill=\"none\" stroke=\"context-stroke\" "
           "stroke-width=\"1.5\" stroke-linecap=\"round\" "
           "stroke-linejoin=\"round\"/></marker></defs>";
    svg += boxes + edgeSvg + nodes + "</svg>";
    return svg;
}

} // namespace viz
