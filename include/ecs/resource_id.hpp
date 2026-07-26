// ecs/resource_id.hpp
//
// ResourceId: the stable, process-local id assigned to each resource type on
// first use (see resource_id<T> in resource.hpp). A separate id space from
// ComponentId -- a strong type so the two can never be crossed, even though a
// component and a resource may share a numeric id.

#pragma once

#include <ecs/strong_id.hpp> // StrongId

#include <cstdint>

namespace ecs {

using ResourceId = StrongId<struct ResourceIdTag, std::uint32_t>;

} // namespace ecs
