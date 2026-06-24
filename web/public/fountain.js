// Shared pieces for the JS-defined fountain demos (index-js.html and
// index-js-mt.html): the component schemas, the per-frame renderer (read the
// JS-defined columns, pack, draw through the host bridge), and a small HUD
// updater. The page bootstraps (app-js.js / app-js-mt.js) add the systems and
// the frame loop -- that is where the two demos differ.

function fountainComponents(w) {
  return {
    Position: w.defineComponent('Position', [{ name: 'x', type: 'f32' }, { name: 'y', type: 'f32' }]),
    Velocity: w.defineComponent('Velocity', [{ name: 'x', type: 'f32' }, { name: 'y', type: 'f32' }]),
    Color:    w.defineComponent('Color',    [{ name: 'r', type: 'f32' }, { name: 'g', type: 'f32' }, { name: 'b', type: 'f32' }]),
    Age:      w.defineComponent('Age',      [{ name: 't', type: 'f32' }, { name: 'max', type: 'f32' }]),
  };
}

// Returns render(): pack Position + Color + Age(life) into the host's scratch
// buffer and draw. No C++ knows these components.
function fountainRenderer(w, C) {
  return function render() {
    var px = w.fieldView(C.Position, 0), py = w.fieldView(C.Position, 1);
    var cr = w.fieldView(C.Color, 0), cg = w.fieldView(C.Color, 1), cb = w.fieldView(C.Color, 2);
    var at = w.fieldView(C.Age, 0), am = w.fieldView(C.Age, 1);
    var n = px ? px.length : 0;
    if (n === 0) { Module.draw_points(0); return; }
    var buf = Module.scratch_view(n); // Float32Array, n*6: x,y,r,g,b,life
    for (var i = 0; i < n; i++) {
      var o = i * 6;
      var life = am[i] > 0 ? Math.max(0, 1 - at[i] / am[i]) : 1;
      buf[o] = px[i]; buf[o + 1] = py[i];
      buf[o + 2] = cr[i]; buf[o + 3] = cg[i]; buf[o + 4] = cb[i]; buf[o + 5] = life;
    }
    Module.draw_points(n);
  };
}

// Returns hud(): refresh fps + particle count ~2.5x/s, and (opts.lanes) the lane
// count once. Call once per frame.
function fountainHud(w, opts) {
  opts = opts || {};
  var frames = 0, t0 = 0, lanesShown = false;
  return function hud() {
    if (opts.lanes && !lanesShown) {
      document.getElementById('lanes').textContent = w.laneCount();
      lanesShown = true;
    }
    frames++;
    var now = performance.now();
    if (t0 === 0) t0 = now;
    if (now - t0 >= 400) {
      document.getElementById('fps').textContent = (frames * 1000 / (now - t0)).toFixed(0);
      document.getElementById('count').textContent = w.entityCount();
      frames = 0; t0 = now;
    }
  };
}
