// ecs/column_id.hpp
//
// ColumnId: a column's slot within an archetype -- its position in the
// archetype's sorted Signature, which is also its position in the parallel
// `columns` vector. Assigned once when the archetype is built (columns are
// created in signature order and never reordered), so it is a stable handle for
// that archetype's lifetime.
//
// A strong type so a column slot can't be crossed with a row index, a component
// id, or an archetype id. Signature::find(ComponentId) returns one; Archetype's
// column vector (an IdVector) is indexed by it.

#pragma once

#include <ecs/detail/strong_id.hpp> // StrongId
#include <cstdint>

namespace ecs {

using ColumnId = StrongId<struct ColumnIdTag, std::uint32_t>;

} // namespace ecs
