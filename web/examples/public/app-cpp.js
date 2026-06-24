// C++-defined fountain, single-threaded (index.html). The sim is C++; this page
// only wires the controls (controls.js) and registers one extra system authored
// here in JavaScript -- a recolor that reads the native Position and writes the
// native Color, leveled into the live C++ schedule. It is gated by the recolor
// checkbox (window.recolorOn from controls.js), so the default fountain is
// unchanged.
function defineRainbow() {
  var C = Module.ecs_components();
  Module.ecs_define_system('rainbow', { read: [C.Position], write: [C.Color] }, function (n, c) {
    if (!recolorOn) return;
    var px = c.Position.x, r = c.Color.r, g = c.Color.g, b = c.Color.b;
    for (var i = 0; i < n; i++) {
      var h = px[i] * 0.5 + 0.5; // x in [-1,1] -> hue phase
      r[i] = 0.5 + 0.5 * Math.sin(6.2832 * h);
      g[i] = 0.5 + 0.5 * Math.sin(6.2832 * h + 2.0944);
      b[i] = 0.5 + 0.5 * Math.sin(6.2832 * h + 4.1888);
    }
  });
}

var Module = {
  canvas: document.getElementById('canvas'),
  print:    function (t) { console.log(t); },
  printErr: function (t) { console.error(t); },
  onRuntimeInitialized: wireControls,
  onEcsReady: defineRainbow,
};
