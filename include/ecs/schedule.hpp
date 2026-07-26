// ecs/schedule.hpp
//
// Umbrella for the access-analyzed, data-parallel system scheduler. The pieces
// live in schedule/ (mirroring event/, parallel/, sync/):
//
//   schedule/access.hpp    phase<N>, SystemId, SystemAccess, conflicts()
//   schedule/events.hpp    sched_event::*, ScheduleEvent
//   schedule/system.hpp    SystemRecord + the system-parameter/kernel protocols
//   schedule/validate.hpp  the consteval checks registration is gated on
//   schedule/work_item.hpp WorkItem/Unit -- one claimable slice of a wave
//   schedule/wave.hpp      the work-item executor (build + dispatch)
//   schedule/graph.hpp     systems -> wavefront levels -> wave plans
//   schedule/schedule.hpp  the Schedule class (registration, waves, run loop)
//
// Include this header; the split is an implementation layout, not an API.

#pragma once

#include <ecs/schedule/schedule.hpp>
