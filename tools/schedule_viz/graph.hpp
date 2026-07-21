// tools/schedule_viz/graph.hpp
//
// Graph algorithms over a schedule, independent of rendering:
//   reduced_dependencies() -- transitively-reduced conflict edges, built from
//                             the scheduler's own Schedule::conflicts predicate
//                             (the same one the wavefront leveling uses).
//   order_phase()          -- barycenter crossing reduction within a phase's
//                             levels, with layer_crossings() as its cost metric.

#pragma once

#include "ecs/schedule.hpp"
#include <algorithm>
#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace viz::detail {

// systems() is an IdVector keyed by SystemId; turn a raw position into that id.
inline ecs::SystemId sid(std::size_t const i) {
    return ecs::SystemId {static_cast<ecs::SystemId::type>(i)};
}

// Transitively-reduced dependency edges (a -> b, a < b within a phase) built
// from ecs::conflicts -- the same predicate the wavefront leveling uses.
inline std::vector<std::pair<std::size_t, std::size_t>> reduced_dependencies(
    ecs::Schedule const& sched) {
    auto const& sys = sched.systems();
    auto const n    = sys.size();
    // Direct conflict edges j -> i (j < i, same phase). Registration order
    // sets the direction.
    std::vector<std::vector<std::size_t>> succ(n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (sys[sid(j)].phase == sys[sid(i)].phase &&
                ecs::conflicts(sys[sid(j)].access, sys[sid(i)].access))
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

// --- crossing reduction (barycenter ordering) -------------------------------
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
inline void order_phase(std::vector<std::vector<std::size_t>>& layers,
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
        std::stable_sort(keyed.begin(), keyed.end(), [](auto const& a, auto const& b) {
            return a.first < b.first;
        });
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

} // namespace viz::detail
