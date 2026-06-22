// tools/schedule_viz.hpp
//
// Standalone visualizer for an ecs::Schedule's wave DAG -- free functions built
// entirely on the *public* Schedule API (systems(), level_count(), and the
// Schedule::conflicts() predicate); the core scheduler carries no visualization
// code.
//
//   #include "schedule_viz.hpp"
//   viz::NameTable nt;
//   nt.component<Position>("Position").resource<Gravity>("Gravity");
//   std::cout << viz::to_svg(sched, nt);   // standalone SVG, no Graphviz
//   std::cout << viz::to_dot(sched, nt);   // Graphviz DOT: | dot -Tsvg
//
// Layout (both back-ends): an outer box per phase, an inner box per conflict
// level (a row of conflict-free systems), one box per system listing its full
// access signature (every component/resource with R/W, plus Commands). Edges
// are transitively reduced; the SVG back-end also runs a barycenter pass to cut
// crossings.
//
// This umbrella just aggregates the pieces under schedule_viz/:
//   text.hpp        XML-escape / width-estimate / number helpers
//   name_table.hpp  viz::NameTable (id -> display name)         [public]
//   node_view.hpp   backend-neutral per-system content model
//   graph.hpp       reduced dependency edges + barycenter ordering
//   svg_node.hpp    per-box SVG rendering + sizing
//   dot.hpp         Graphviz-DOT back-end (viz::to_dot)         [public]
//   svg.hpp         direct-SVG back-end (viz::to_svg)           [public]

#pragma once

#include "schedule_viz/dot.hpp"
#include "schedule_viz/name_table.hpp"
#include "schedule_viz/svg.hpp"
