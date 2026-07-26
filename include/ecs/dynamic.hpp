// ecs/dynamic.hpp
//
// Umbrella for the runtime-described component module: components whose
// layout is known only at run time (a field list built from a schema, a
// script, or a JS host) rather than from a C++ struct at compile time.
//
//   dynamic/component_desc.hpp  FieldType/Field/ComponentDesc -- the layout
//   dynamic/dynamic_column.hpp  DynamicColumn -- storage for one such component
//   dynamic/registry.hpp        Registry -- name -> ComponentId, descs by id
//   dynamic/native.hpp          register_native<T>() -- describe a C++ struct
//                               through the same path, via reflection
//   dynamic/world_ops.hpp       WorldOps -- spawn/add/remove/set by ComponentId
//
// This module is deliberately NOT part of ecs/ecs.hpp: a program that only
// uses statically-typed components should not pay for the registry or the
// type-erased column. Include this header to opt in.

#pragma once

#include <ecs/dynamic/component_desc.hpp>
#include <ecs/dynamic/dynamic_column.hpp>
#include <ecs/dynamic/native.hpp>
#include <ecs/dynamic/registry.hpp>
#include <ecs/dynamic/world_ops.hpp>
