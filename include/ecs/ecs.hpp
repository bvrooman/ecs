// ecs/ecs.hpp -- single include for the whole library.
#pragma once

#include "archetype.hpp"
#include "command_buffer.hpp"
#include "entity.hpp"
#include "parallel/worker_pool.hpp"
#include "query.hpp"
#include "reflection/reflect.hpp"
#include "reflection/type_names.hpp"
#include "resource.hpp"
#include "schedule.hpp"
#include "soa.hpp"
#include "sync/snapshot_channel.hpp"
#include "sync/triple_buffer.hpp"
#include "world.hpp"
