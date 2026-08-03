// ecs/schedule/params.hpp
//
// Umbrella for the system parameters, both kinds, and the protocol substrate
// behind them. The two policies are params/parallel.hpp (parallel_param, for
// add_parallel systems) and params/serial.hpp (serial_param, for
// add_serial); params/protocol.hpp holds the whole-parameter-list drivers,
// which are generic over the policy and serve both.
//
// Most of the parameters are parallel-only -- the parallel-primitives suite.
// One mechanism wears every face: per-item private SLOTS (indexed by the
// item's ordinal in canonical generation order) plus optional deterministic
// barrier-time prepare/finish hooks. params/parallel.hpp holds that substrate;
// the per-primitive headers hold each face:
//
//   Reduce<T, Op>   fold-shaped state (sums, bounds, grids): private partials
//                   folded into the T resource in ordinal order at the barrier
//   Extract<T>      gather where output == matched rows: disjoint spans over a
//                   pre-sized vector-like T resource, no fold at all
//   Collect<T>      filtered gather (output size unknown): private partials
//                   concatenated into the T resource in ordinal order
//   EventWriter<T>  emit into a double-buffered Events<T> channel resource;
//   EventReader<T>  read everything emitted during the PREVIOUS tick
//   Bin<V>          group-by: (bucket, value) pairs counting-sorted into
//                   contiguous per-bucket spans of a Bins<V> resource
//   Scratch<T>      per-item temp workspace, reset keeping capacity, no access
//   Random          per-item deterministic PCG32 stream, seeded from
//                   (RandomSeed resource, schedule tick, system id, ordinal)
//   Commands&       in a parallel system: per-item private stores, enqueued
//                   wave's flush in ordinal order (canonical replay)
//
// Every merge runs in canonical world order, so a schedule's observable
// results are bitwise identical at 1 lane and N lanes -- including float
// reductions.
//
// The serial side is smaller: Local<T> (params/local.hpp) is per-system
// state for add_serial(), and is the only parameter exclusive to it.

#pragma once

#include <ecs/schedule/params/bin.hpp>
#include <ecs/schedule/params/collect.hpp>
#include <ecs/schedule/params/commands.hpp>
#include <ecs/schedule/params/events.hpp>
#include <ecs/schedule/params/extract.hpp>
#include <ecs/schedule/params/fold.hpp>
#include <ecs/schedule/params/local.hpp>
#include <ecs/schedule/params/parallel.hpp>
#include <ecs/schedule/params/protocol.hpp>
#include <ecs/schedule/params/query.hpp>
#include <ecs/schedule/params/random.hpp>
#include <ecs/schedule/params/reduce.hpp>
#include <ecs/schedule/params/resource.hpp>
#include <ecs/schedule/params/scratch.hpp>
#include <ecs/schedule/params/serial.hpp>
#include <ecs/schedule/params/state.hpp>
#include <ecs/schedule/params/world_view.hpp>
