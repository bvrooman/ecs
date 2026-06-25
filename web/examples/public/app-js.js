// Single-threaded JS-defined fountain (index-js.html). Components and the
// renderer come from fountain.js; the systems and the frame loop are defined
// here. Everything runs main-thread.
function setup() {
    var w = new Module.DynamicWorld();
    var C = fountainComponents(w);

    // Sim parameters (plain JS state the kernels close over). DT is refreshed from
    // the real frame dt each tick (see onFrame) so motion and lifetime are
    // frame-rate independent, matching app-js-mt; the 0.016 is just the seed.
    var DT = 0.016, G = -1.7, RATE = 9, NX = 0.0, NY = -0.85;
    
    // emit: command-only system, spawns RATE particles/tick from the nozzle.
    w.defineSystem('emit', {commands: true}, function () {
        for (var i = 0; i < RATE; i++) {
            var warm = Math.random();
            w.spawn([
                [C.Position, [NX, NY]],
                [C.Velocity, [(Math.random() - 0.5) * 0.5, 1.2 + Math.random() * 1.4]],
                [C.Color, [1.0, 0.5 + 0.35 * warm, 0.1 + 0.15 * warm]],
                [C.Age, [0.0, 1.8 + Math.random()]]
            ]);
        }
    });
    // gravity: write Velocity.
    w.defineSystem('gravity', {write: [C.Velocity]}, function (n, v) {
        var vy = v.Velocity.y;
        for (var i = 0; i < n; i++) vy[i] += G * DT;
    });
    // integrate: read Velocity (written by gravity -> later wave), write Position.
    w.defineSystem('integrate', {write: [C.Position], read: [C.Velocity]}, function (n, v) {
        var px = v.Position.x, py = v.Position.y, vx = v.Velocity.x, vy = v.Velocity.y;
        for (var i = 0; i < n; i++) {
            px[i] += vx[i] * DT;
            py[i] += vy[i] * DT;
        }
    });
    // age + reap: advance Age, destroy the expired ones via the entity handles.
    w.defineSystem('age', {write: [C.Age], commands: true}, function (n, v, ent) {
        var t = v.Age.t, max = v.Age.max;
        for (var i = 0; i < n; i++) {
            t[i] += DT;
            if (t[i] >= max[i]) w.destroy(ent[2 * i], ent[2 * i + 1]);
        }
    });

    var render = fountainRenderer(w, C);
    var hud = fountainHud(w);
    var STATS = w.statsOn(); // ?stats -> log the per-tick timing distribution
    Module.onFrame = function (dt) {
        DT = Math.min(dt, 0.05); // real frame dt (clamped), like app-js-mt's P.dt
        w.tick();
        if (STATS) {
            var s = w.pollStats();
            if (s) console.log(s);
        }
        render();
        hud();
    };
}

var Module = {
    canvas: document.getElementById('canvas'),
    print: function (t) {
        console.log(t);
    },
    printErr: function (t) {
        console.error(t);
    },
    onRuntimeInitialized: setup, // build the JS world before main() starts the loop
};
