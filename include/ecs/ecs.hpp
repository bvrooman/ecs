// ecs/ecs.hpp -- single include for the whole library.
#pragma once

#include <ecs/archetype.hpp>
#include <ecs/command_buffer.hpp>
#include <ecs/entity.hpp>
#include <ecs/event/emitter.hpp>
#include <ecs/event/observer.hpp>
#include <ecs/parallel/worker_pool.hpp>
#include <ecs/query.hpp>
#include <ecs/reflection/reflect.hpp>
#include <ecs/reflection/type_names.hpp>
#include <ecs/resource.hpp>
#include <ecs/schedule.hpp>
#include <ecs/soa.hpp>
#include <ecs/sync/snapshot_channel.hpp>
#include <ecs/sync/triple_buffer.hpp>
#include <ecs/world.hpp>
