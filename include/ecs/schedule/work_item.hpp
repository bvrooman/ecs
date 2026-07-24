#pragma once

#include "ecs/archetype_id.hpp"
#include "ecs/system_id.hpp"

namespace ecs::detail {

struct Unit {
    ArchetypeId archetype;
    std::uint32_t begin;
    std::uint32_t end;
};

// One unit of claimable work: a row-range of one kernel system's matched
// archetype, or (archetype == kImperative) an entire opaque system. `ordinal`
// is the item's index within ITS SYSTEM in generation order (archetypes
// ascending, rows ascending -- the serial-walk order); it survives the LPT
// sort and indexes the per-item slots of stateful kernel parameters, and
// barrier-time folds walk ordinals so results are canonical-ordered no matter
// which lane ran what.
struct WorkItem {
    SystemId system; // index into the schedule's system list
    std::vector<Unit> units;
    std::uint32_t begin;
    std::uint32_t end;
    std::uint32_t ordinal;
    double busy_us = 0;
};
}