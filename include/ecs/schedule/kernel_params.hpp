// ecs/schedule/kernel_params.hpp
//
// Umbrella for the kernel-only system parameters -- the parallel-primitives
// suite -- and the slot substrate behind them.
//
// One mechanism wears every face: per-item private SLOTS (indexed by the
// item's ordinal in canonical generation order) plus optional deterministic
// barrier-time prepare/finish hooks. See params/protocol.hpp for the
// substrate and the per-primitive headers for each face:
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
//   Commands&       in a kernel: per-item private stores, enqueued into the
//                   wave's flush in ordinal order (canonical replay)
//
// Every merge runs in canonical world order, so a schedule's observable
// results are bitwise identical at 1 lane and N lanes -- including float
// reductions.
//
// params/local.hpp is the one non-kernel face here: Local<T> per-system state
// for imperative add()/add_once() systems, plus the imperative-parameter
// protocol Schedule::add compiles against.

#pragma once

#include <ecs/schedule/params/protocol.hpp>

#include <ecs/schedule/params/bin.hpp>
#include <ecs/schedule/params/collect.hpp>
#include <ecs/schedule/params/commands.hpp>
#include <ecs/schedule/params/events.hpp>
#include <ecs/schedule/params/extract.hpp>
#include <ecs/schedule/params/local.hpp>
#include <ecs/schedule/params/random.hpp>
#include <ecs/schedule/params/reduce.hpp>
#include <ecs/schedule/params/scratch.hpp>
