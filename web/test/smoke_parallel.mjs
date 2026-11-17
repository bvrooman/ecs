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

// --- JS serial systems that spawn, in a wave shared with a parallel system ---
// The emitters' work items sit in the same wave as the parallel system's
// slices, so any lane can claim them. Each spawns tagged rows through the
// deferred spawn(); the rows must apply in registration order (e0, e1, e2, e3)
// every tick, whichever lane ran which emitter.
const w2 = new M.DynamicWorld();
const Tag = w2.defineComponent('Tag', [{ name: 'v', type: 'f32' }]);
const Busy = w2.defineComponent('Busy', [{ name: 'x', type: 'f32' }]);
for (let i = 0; i < N; i++) { const e = w2.createEntity(); w2.addComponent(e, Busy, [i]); }
w2.defineParallelSystem('busy', { write: [Busy], params: { k: 1 } },
  (lo, hi, c, p) => { for (let i = lo; i < hi; i++) c.Busy.x[i] += p.k; });
const EMITTERS = 4, PER = 3, TICKS = 5;
for (let k = 0; k < EMITTERS; k++)
  w2.defineSystem(`e${k}`, { commands: true }, () => {
    for (let i = 0; i < PER; i++) w2.spawn([[Tag, [k * 100 + i]]]);
  });
check(w2.waveCount() === 1, `emitters share one wave with the parallel system (${w2.waveCount()} waves)`);

let ran = true;
try {
  for (let t = 0; t < TICKS; t++) w2.tick();
} catch (err) {
  ran = false;
  console.log('tick threw: ' + err);
}
check(ran, `${TICKS} ticks with JS emitters in a multi-lane wave ran`);
const spawned = EMITTERS * PER * TICKS;
check(w2.entityCount() === N + spawned, `all ${spawned} emitted rows applied (entityCount ${w2.entityCount() - N})`);
const expect = [];
for (let t = 0; t < TICKS; t++)
  for (let k = 0; k < EMITTERS; k++)
    for (let i = 0; i < PER; i++) expect.push(k * 100 + i);
const tags = w2.fieldView(Tag, 0);
const got = tags ? Array.from(tags) : [];
check(JSON.stringify(got) === JSON.stringify(expect),
  `emitted rows in registration order every tick (first 12: ${got.slice(0, 12)})`);

console.log(`${checks - fails}/${checks} checks passed`);
process.exit(fails === 0 ? 0 : 1);
