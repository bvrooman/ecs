// web/dynamic/smoke.mjs
//
// Phase A interop smoke test: define components, create entities, and add / get
// / set / remove / destroy them entirely from JavaScript, then prove a
// zero-copy typed-array view aliases the engine's SoA storage. Run under node:
//   node smoke_dynamic.mjs   (copied next to ecs_dynamic.mjs by the build)
import createModule from './ecs_dynamic.mjs';

const M = await createModule();

let checks = 0, fails = 0;
const check = (cond, msg) => {
  checks++;
  if (!cond) { fails++; console.error(`  FAIL: ${msg}`); }
};

// --- components defined from JS ---------------------------------------------
const w = new M.DynamicWorld();
const Pos = w.defineComponent('Position', [{ name: 'x', type: 'f32' }, { name: 'y', type: 'f32' }]);
const Vel = w.defineComponent('Velocity', [{ name: 'x', type: 'f32' }, { name: 'y', type: 'f32' }]);
console.log(`defined Position=${Pos}, Velocity=${Vel}`);

// --- add / get / set round-trip ---------------------------------------------
const e = w.createEntity();
w.addComponent(e, Pos, [1.5, -2.5]);
w.addComponent(e, Vel, [0.1, 0.2]);
check(w.hasComponent(e, Pos), 'entity has Position');
check(w.hasComponent(e, Vel), 'entity has Velocity');

let p = w.getComponent(e, Pos);
check(p !== null && p[0] === 1.5 && p[1] === -2.5, `get Position == [1.5,-2.5] (got ${p})`);

w.setComponent(e, Pos, [9, 9]);
p = w.getComponent(e, Pos);
check(p[0] === 9 && p[1] === 9, `set Position -> [9,9] (got ${p})`);

// --- remove moves the entity between archetypes -----------------------------
w.removeComponent(e, Vel);
check(!w.hasComponent(e, Vel), 'Velocity removed');
check(w.hasComponent(e, Pos), 'Position survives the relocation');
check(w.getComponent(e, Vel) === null, 'get on a missing component returns null');
check(w.entityCount() === 1, `entityCount == 1 (got ${w.entityCount()})`);

// --- zero-copy typed-array view (fresh world -> one clean {Position} archetype) -
const w2 = new M.DynamicWorld(); // reuses the globally-defined component ids
const N = 6;
const ents = [];
for (let i = 0; i < N; i++) {
  const en = w2.createEntity();
  w2.addComponent(en, Pos, [i, i * 2]);
  ents.push(en);
}
check(w2.entityCount() === N, `w2 entityCount == ${N} (got ${w2.entityCount()})`);

const xs = w2.fieldView(Pos, 0); // Float32Array aliasing Position.x across the archetype
check(xs instanceof Float32Array, `fieldView returns Float32Array (got ${xs && xs.constructor.name})`);
check(xs.length === N, `view length == ${N} (got ${xs.length})`);
check(Array.from(xs).every((v, i) => v === i), `view reads scattered values (got ${Array.from(xs)})`);

for (let i = 0; i < N; i++) xs[i] = i * 10; // write through the view
let aliasOk = true;
for (let i = 0; i < N; i++) if (w2.getComponent(ents[i], Pos)[0] !== i * 10) aliasOk = false;
check(aliasOk, 'write-through the view is visible via getComponent (zero-copy aliasing)');

// --- destroy + swap-remove consistency --------------------------------------
w2.destroyEntity(ents[1]);
check(!w2.hasComponent(ents[1], Pos), 'destroyed entity is gone');
check(w2.entityCount() === N - 1, `entityCount after destroy == ${N - 1} (got ${w2.entityCount()})`);
check(w2.getComponent(ents[0], Pos)[0] === 0, 'survivor es[0] keeps its value');
check(w2.getComponent(ents[N - 1], Pos)[0] === (N - 1) * 10, 'swap-relocated survivor keeps its value');

// --- Phase B: systems defined in JavaScript ---------------------------------
console.log('--- JS-defined systems ---');
const w3 = new M.DynamicWorld(); // reuses the globally-defined Position/Velocity
const movers = [];
for (let i = 0; i < 4; i++) {
  const en = w3.createEntity();
  w3.addComponent(en, Pos, [0, 0]);
  w3.addComponent(en, Vel, [3, 0]);
  movers.push(en);
}
const fixed = w3.createEntity();    // Position only -> the integrate query must skip it
w3.addComponent(fixed, Pos, [0, 0]);

// gravity writes Velocity; integrate reads Velocity and writes Position. The
// shared Velocity access makes the scheduler run integrate *after* gravity.
// (dt = 1, g = -2 -> integer arithmetic, exact in f32.)
w3.defineSystem('gravity', { write: [Vel] }, (n, c) => {
  const vy = c.Velocity.y;
  for (let i = 0; i < n; i++) vy[i] += -2;
});
w3.defineSystem('integrate', { write: [Pos], read: [Vel] }, (n, c) => {
  const px = c.Position.x, py = c.Position.y, vx = c.Velocity.x, vy = c.Velocity.y;
  for (let i = 0; i < n; i++) { px[i] += vx[i]; py[i] += vy[i]; }
});

check(w3.waveCount() === 2, `scheduler leveled the systems into 2 waves (got ${w3.waveCount()})`);

w3.tick();
const tick1Ok = movers.every((e) => {
  const p = w3.getComponent(e, Pos), v = w3.getComponent(e, Vel);
  return p[0] === 3 && p[1] === -2 && v[0] === 3 && v[1] === -2;
});
check(tick1Ok, 'after 1 tick Pos=[3,-2], Vel=[3,-2] (integrate saw post-gravity velocity)');

w3.tick();
const tick2Ok = movers.every((e) => {
  const p = w3.getComponent(e, Pos), v = w3.getComponent(e, Vel);
  return p[0] === 6 && p[1] === -6 && v[0] === 3 && v[1] === -4;
});
check(tick2Ok, 'after 2 ticks Pos=[6,-6], Vel=[3,-4]');

const fp = w3.getComponent(fixed, Pos);
check(fp[0] === 0 && fp[1] === 0, 'Position-only entity skipped by the {Position,Velocity} query');

console.log(`${checks - fails}/${checks} checks passed`);
process.exit(fails === 0 ? 0 : 1);
