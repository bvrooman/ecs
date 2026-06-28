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
    var P = {dt: 1 / 120, gravity: -1.7, time: 0}; // dt is a FIXED 120 Hz step (see onFrame)

    // --- STRUCTURAL systems: main-thread (spawn/destroy, close over JS) ---
    w.defineSystem('emit', {commands: true}, function () {
        for (var i = 0; i < RATE; i++) {
            var warm = Math.random();
            w.spawn([
                [C.Position, [0, -0.85]],
                [C.Velocity, [(Math.random() - 0.5) * 0.5, 1.2 + Math.random() * 1.4]],
                [C.Color, [1.0, 0.5 + 0.35 * warm, 0.1 + 0.15 * warm]],
                [C.Age, [0.0, 1.8 + Math.random()]]
            ]);
        }
    });
    // --- HOT DATA systems: PARALLEL across worker lanes (pure kernels) ---
    w.defineParallelSystem('gravity', {write: [C.Velocity], params: P},
        function (lo, hi, c, p) {
            var vy = c.Velocity.y;
            for (var i = lo; i < hi; i++) vy[i] += p.gravity * p.dt;
        });
    // w.defineParallelSystem('turbulence', {read: [C.Position], write: [C.Velocity], params: P},
    //     function (lo, hi, c, p) {
    //         for (var i = lo; i < hi; i++)
    //             c.Velocity.x[i] += Math.sin(c.Position.y[i] * 3.0 + p.time * 1.5) * 0.7 * p.dt;
    //     });
    w.defineParallelSystem('integrate', {write: [C.Position], read: [C.Velocity], params: P},
        function (lo, hi, c, p) {
            for (var i = lo; i < hi; i++) {
                c.Position.x[i] += c.Velocity.x[i] * p.dt;
                c.Position.y[i] += c.Velocity.y[i] * p.dt;
            }
        });
    w.defineSystem('age', {write: [C.Age], commands: true}, function (n, c, ent) {
        for (var i = 0; i < n; i++) {
            c.Age.t[i] += P.dt;
            if (c.Age.t[i] >= c.Age.max[i]) w.destroy(ent[2 * i], ent[2 * i + 1]);
        }
    });

    var render = fountainRenderer(w, C);
    var hud = fountainHud(w, {lanes: true});
    var STATS = w.statsOn(); // ?stats -> log the per-tick timing distribution
    var acc = 0; // unspent real time, drained in fixed P.dt steps
    Module.onFrame = function (dt) {
        acc += Math.min(dt, 0.05); // clamp the catch-up after a tab-switch/stall
        // Fixed-timestep accumulator: run as many fixed P.dt ticks as the elapsed
        // real time calls for, so sim time stays independent of the render rate.
        for (var steps = 0; acc >= P.dt && steps < 8; steps++) {
            P.time += P.dt;
            w.tick(); // emit/age on main thread; gravity/integrate fan out to lanes
            acc -= P.dt;
        }
        if (STATS) {
            var s = w.pollStats();
            if (s) console.log(s);
        }
        render();
        hud();
    };
}

if (!self.crossOriginIsolated) document.getElementById('coiWarn').style.display = '';

var Module = {
    canvas: document.getElementById('canvas'),
    print: function (t) {
        console.log(t);
    },
    printErr: function (t) {
        console.error(t);
    },
    onRuntimeInitialized: setup,
};
