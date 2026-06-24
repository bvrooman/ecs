// Shared control wiring for the C++-defined particle demos (index.html and
// index-mt.html). Sliders/buttons call the EM_ASM-exported Module._ecs_* setters.
// The worker-lanes slider and the recolor checkbox are wired only if the page has
// them (feature-detected), so one file serves both pages. `recolorOn` is a global
// the recolor JS system (app-cpp.js) reads.
var recolorOn = false;

function wireControls() {
  var call = function (name, args) { if (window.Module && Module[name]) Module[name].apply(null, args); };
  var $ = function (id) { return document.getElementById(id); };

  var g = $('gravity'), gv = $('gravityVal');
  g.addEventListener('input', function () {
    gv.textContent = parseFloat(g.value).toFixed(1);
    call('_ecs_set_gravity', [parseFloat(g.value)]);
  });

  var em = $('emit'), ev = $('emitVal');
  em.addEventListener('input', function () {
    ev.textContent = em.value;
    call('_ecs_set_emit_rate', [parseInt(em.value, 10)]);
  });

  var paused = false, pauseBtn = $('pause');
  pauseBtn.addEventListener('click', function () {
    paused = !paused;
    call('_ecs_set_paused', [paused ? 1 : 0]);
    pauseBtn.textContent = paused ? 'resume' : 'pause';
  });

  $('reset').addEventListener('click', function () { call('_ecs_reset', []); });

  // Cursor over the canvas aims the nozzle (clip space, y up); leaving snaps it
  // back to the default bottom-centre origin.
  var cv = $('canvas');
  cv.addEventListener('pointermove', function (e) {
    var r = cv.getBoundingClientRect();
    var cx = ((e.clientX - r.left) / r.width) * 2 - 1;
    var cy = 1 - ((e.clientY - r.top) / r.height) * 2;
    call('_ecs_set_nozzle', [cx, cy]);
  });
  cv.addEventListener('pointerleave', function () { call('_ecs_set_nozzle', [0.0, -0.85]); });

  // Multi-threaded page only: worker lanes. Update the label live, rebuild the
  // pool on release (each rebuild joins + respawns resident worker threads).
  var lc = $('lanesCtl');
  if (lc) {
    var lcv = $('lanesCtlVal');
    lc.addEventListener('input', function () { lcv.textContent = lc.value; });
    lc.addEventListener('change', function () { call('_ecs_set_lanes', [parseInt(lc.value, 10)]); });
  }

  // Single-threaded page only: the recolor toggle drives the recolor JS system.
  var recolor = $('recolor');
  if (recolor) recolor.addEventListener('change', function () { recolorOn = recolor.checked; });
}
