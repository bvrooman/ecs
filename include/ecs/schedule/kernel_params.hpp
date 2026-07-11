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
//   Scratch<T>      per-item temp workspace, reset keeping capacity, no access
//   Random          per-item deterministic PCG32 stream, seeded from
//                   (RandomSeed resource, schedule tick, system id, ordinal)
//
// Every merge runs in canonical world order, so a schedule's observable
// results are bitwise identical at 1 lane and N lanes -- including float
// reductions.

#pragma once

#include "params/protocol.hpp"

#include "params/extract.hpp"
#include "params/random.hpp"
#include "params/reduce.hpp"
#include "params/scratch.hpp"
