// web/dynamic/smoke_parallel.mjs
//
// -pthread node smoke: defineParallelSystem actually fans the pure JS kernel out
// across WorkerPool lanes (Web Worker threads), each eval'ing the source in its
// own context and writing its slice of the shared component column. Proof =
// a multi-lane pool (laneCount > 1) + a correct result over a large archetype.

import Module from './ecs_dynamic_mt.mjs';

const M = await Module();

let checks = 0, fails = 0;
function check(cond, msg) {
  checks++;
  if (!cond) { fails++; console.log('FAIL: ' + msg); }
  else console.log('ok: ' + msg);
}

const w = new M.DynamicWorld();
check(w.laneCount() >= 2, `resident WorkerPool has ${w.laneCount()} lanes (multi-threaded)`);

const Val = w.defineComponent('Val', [{ name: 'x', type: 'f32' }]);
const N = 4000;
const ents = [];
for (let i = 0; i < N; i++) { const e = w.createEntity(); w.addComponent(e, Val, [i]); ents.push(e); }

// pure kernel: x *= k, over [lo,hi); each lane eval's it and runs its own slice.
const params = { k: 3 };
w.defineParallelSystem('scale', { write: [Val], params: params },
  (lo, hi, c, p) => { for (let i = lo; i < hi; i++) c.Val.x[i] *= p.k; });

w.tick(); // dispatched across the worker lanes

let ok = true;
for (let i = 0; i < N; i++) if (w.getComponent(ents[i], Val)[0] !== i * 3) { ok = false; break; }
check(ok, `all ${N} Val.x scaled by p.k=3 across worker lanes (every slice correct)`);

console.log(`${checks - fails}/${checks} checks passed`);
process.exit(fails === 0 ? 0 : 1);
