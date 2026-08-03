// ecs/schedule/access.hpp
//
// What ORDERS systems: the derived access sets (SystemAccess) and the conflict
// predicate the wavefront leveling and the visualizer both apply to them. The
// registration options live in options.hpp.

#pragma once

#include <ecs/component_id.hpp> // ComponentId
#include <ecs/resource_id.hpp>  // ResourceId
#include <ecs/system_id.hpp>    // SystemId

#include <algorithm>
#include <vector>

namespace ecs {

// A system's derived component/resource access (id sets). `exclusive` means the
// system has unanalyzable access (it took a raw World&) and conflicts with
// every other system. `reads_all` means it reads everything (a WorldView): it
// conflicts only with writers, so read-only systems still run in parallel.
struct SystemAccess {
    std::vector<ComponentId> reads, writes;
    std::vector<ResourceId> res_reads, res_writes, res_folds;
    bool exclusive = false;
    bool reads_all = false;
    // Purely informational (does not affect conflict analysis): the system took
    // a Commands& and may record deferred structural edits.
    bool commands = false;
};

namespace detail {

    // Access id lists are kept sorted + deduplicated (normalize_access is run
    // at registration), so every conflict check is a linear merge rather than
    // a quadratic scan. Templated over the id type (ComponentId or ResourceId)
    // so a component list can never be merged against a resource list by
    // mistake -- the two spaces are checked independently below.
    inline void normalize_access(SystemAccess& a) {
        auto norm = [](auto& v) {
            std::ranges::sort(v);
            v.erase(std::ranges::unique(v).begin(), v.end());
        };
        norm(a.reads);
        norm(a.writes);
        norm(a.res_reads);
        norm(a.res_writes);
        norm(a.res_folds);
    }

    template <class Id>
    inline bool intersects(std::vector<Id> const& a, std::vector<Id> const& b) {
        auto ia = a.begin();
        auto ib = b.begin();
        while (ia != a.end() && ib != b.end()) {
            if (*ia < *ib)
                ++ia;
            else if (*ib < *ia)
                ++ib;
            else
                return true;
        }
        return false;
    }

    inline bool component_conflicts(SystemAccess const& a, SystemAccess const& b) {
        return intersects(a.writes, b.reads) || intersects(a.writes, b.writes) ||
               intersects(b.writes, a.reads);
    }

    // A fold behaves as a write against reads and plain writes; fold-vs-fold is
    // the one pair that does not conflict.
    inline bool res_conflicts(SystemAccess const& a, SystemAccess const& b) {
        return intersects(a.res_writes, b.res_reads) ||
               intersects(a.res_folds, b.res_reads) ||
               intersects(b.res_writes, a.res_reads) ||
               intersects(b.res_folds, a.res_reads) ||
               intersects(a.res_writes, b.res_writes) ||
               intersects(a.res_writes, b.res_folds) ||
               intersects(b.res_writes, a.res_folds);
    }

    inline bool writes_any(SystemAccess const& a) {
        return !a.writes.empty() || !a.res_writes.empty() || !a.res_folds.empty();
    }

} // namespace detail

// Do two access sets conflict? One writes what the other reads or writes
// (read/read never conflicts); an `exclusive` set conflicts with everything,
// a `reads_all` set only with writers. Components and resources are checked
// independently. Reachable outside detail so the schedule visualizer in tools/
// can rebuild the same dependency graph without re-deriving it.
inline bool conflicts(SystemAccess const& a, SystemAccess const& b) {
    if (a.exclusive || b.exclusive)
        return true;
    if ((a.reads_all && detail::writes_any(b)) || (b.reads_all && detail::writes_any(a)))
        return true;
    return detail::component_conflicts(a, b) || detail::res_conflicts(a, b);
}

} // namespace ecs
