// Parallel JS-defined fountain (index-js-mt.html). Components and the renderer
// come from fountain.js. The hot data systems (gravity, turbulence, integrate)
// are defineParallelSystem -- pure kernels that fan out across the worker lanes;
// the structural ones (emit, age+reap) stay main-thread.
function setup() {
  if (!self.crossOriginIsolated) {
    document.getElementById('coiWarn').style.display = '';
    return; // no SharedArrayBuffer -> the WorkerPool can't start
  }
  var w = new Module.DynamicWorld();
  var C = fountainComponents(w);

  var RATE = 9;
  var P = { dt: 0.016, gravity: -1.7, time: 0 }; // params the kernels read as `p`

  // --- STRUCTURAL systems: main-thread (spawn/destroy, close over JS) ---
  w.defineSystem('emit', { commands: true }, function () {
    for (var i = 0; i < RATE; i++) {
      var warm = Math.random();
      w.spawn([[C.Position, [0, -0.85]],
               [C.Velocity, [(Math.random() - 0.5) * 0.5, 1.2 + Math.random() * 1.4]],
               [C.Color,    [1.0, 0.5 + 0.35 * warm, 0.1 + 0.15 * warm]],
               [C.Age,      [0.0, 1.8 + Math.random()]]]);
    }
  });
  w.defineSystem('age', { write: [C.Age], commands: true }, function (n, c, ent) {
    for (var i = 0; i < n; i++) {
      c.Age.t[i] += P.dt;
      if (c.Age.t[i] >= c.Age.max[i]) w.destroy(ent[2 * i], ent[2 * i + 1]);
    }
  });

  // --- HOT DATA systems: PARALLEL across worker lanes (pure kernels) ---
  w.defineParallelSystem('gravity', { write: [C.Velocity], params: P },
    function (lo, hi, c, p) {
      for (var i = lo; i < hi; i++) c.Velocity.y[i] += p.gravity * p.dt;
    });
  w.defineParallelSystem('turbulence', { read: [C.Position], write: [C.Velocity], params: P },
    function (lo, hi, c, p) {
      for (var i = lo; i < hi; i++)
        c.Velocity.x[i] += Math.sin(c.Position.y[i] * 3.0 + p.time * 1.5) * 0.7 * p.dt;
    });
  w.defineParallelSystem('integrate', { write: [C.Position], read: [C.Velocity], params: P },
    function (lo, hi, c, p) {
      for (var i = lo; i < hi; i++) {
        c.Position.x[i] += c.Velocity.x[i] * p.dt;
        c.Position.y[i] += c.Velocity.y[i] * p.dt;
      }
    });

  var render = fountainRenderer(w, C);
  var hud = fountainHud(w, { lanes: true });
  Module.onFrame = function (dt) {
    P.dt = Math.min(dt, 0.05);
    P.time += P.dt;
    w.tick();   // emit/age on main thread; gravity/turbulence/integrate fan out
    render();
    hud();
  };
}

if (!self.crossOriginIsolated) document.getElementById('coiWarn').style.display = '';

var Module = {
  canvas: document.getElementById('canvas'),
  print:    function (t) { console.log(t); },
  printErr: function (t) { console.error(t); },
  onRuntimeInitialized: setup,
};
