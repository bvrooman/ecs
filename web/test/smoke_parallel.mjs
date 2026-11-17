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

// --- JS emitters in the -pthread build ---------------------------------------
// A JS system's kernel can only run on the main thread, and nothing yet keeps a
// dynamic serial item there: in a wave with other work items, a worker lane can
// claim it and the tick never returns. Until that is fixed, keep each emitter
// alone in its wave -- a one-item wave runs inline on the calling thread -- by
// having them all write the same component, so they level into consecutive
// waves. Their spawns still go through the per-item store and the splice.
const w2 = new M.DynamicWorld();
const Tag = w2.defineComponent('Tag', [{ name: 'v', type: 'f32' }]);
const Gate = w2.defineComponent('Gate', [{ name: 'n', type: 'f32' }]);
w2.addComponent(w2.createEntity(), Gate, [0]); // one row: each kernel runs once a tick
const EMITTERS = 4, PER = 3, TICKS = 3;
for (let k = 0; k < EMITTERS; k++)
  w2.defineSystem(`e${k}`, { write: [Gate], commands: true }, () => {
    for (let i = 0; i < PER; i++) w2.spawn([[Tag, [k * 100 + i]]]);
  });
check(w2.waveCount() === EMITTERS, `each emitter in its own wave (${w2.waveCount()} waves)`);

w2.spawn([[Tag, [-1]]]); // host code between ticks
for (let t = 0; t < TICKS; t++) w2.tick();
const spawned = 1 + EMITTERS * PER * TICKS;
check(w2.entityCount() === 1 + spawned, `all ${spawned} spawned rows applied (entityCount ${w2.entityCount()})`);
const expect = [-1];
for (let t = 0; t < TICKS; t++)
  for (let k = 0; k < EMITTERS; k++)
    for (let i = 0; i < PER; i++) expect.push(k * 100 + i);
const tags = w2.fieldView(Tag, 0);
const got = tags ? Array.from(tags) : [];
check(JSON.stringify(got) === JSON.stringify(expect),
  `host spawn, then e0..e3 each tick (first 13: ${got.slice(0, 13)})`);

console.log(`${checks - fails}/${checks} checks passed`);
process.exit(fails === 0 ? 0 : 1);
