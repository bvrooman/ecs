// ecs/schedule.hpp
//
// Umbrella for the access-analyzed, data-parallel system scheduler. The pieces
// live in schedule/ (mirroring event/, parallel/, sync/), in dependency order:
//
//   schedule/options.hpp   phase/every/times/grain -- the registration options
//   schedule/access.hpp    SystemAccess + conflicts(): what ORDERS systems
//   schedule/wave_id.hpp   WaveId -- a wave's position in the tick's plan order
//   schedule/events.hpp    sched_event::*, ScheduleEvent
//   schedule/system.hpp    SystemRecord + system_param, the per-parameter
//                          binding protocol both system kinds route through
//   schedule/validate.hpp  the consteval checks registration is gated on
//   schedule/wave.hpp      the work-item executor (build + dispatch)
//   schedule/graph.hpp     systems -> wavefront levels -> wave plans
//   schedule/params.hpp    umbrella over params/ -- the system parameters and
//                          the two policies (parallel_param, serial_param)
//   schedule/schedule.hpp  the Schedule class (registration, waves, run loop)
//
// Include this header; the split is an implementation layout, not an API.

#pragma once

#include <ecs/schedule/schedule.hpp>
