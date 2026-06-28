// C++-defined fountain, multi-threaded (index-mt.html). Same C++ sim as the
// single-threaded page but compiled -pthread, so the data-parallel field systems
// fan out across the WorkerPool's Web Worker lanes. This page only wires the
// controls (controls.js, incl. the worker-lanes slider) and shows the COI warning
// if SharedArrayBuffer is unavailable.
if (!self.crossOriginIsolated) document.getElementById('coiWarn').style.display = '';

var Module = {
  canvas: document.getElementById('canvas'),
  print:    function (t) { console.log(t); },
  printErr: function (t) { console.error(t); },
  onRuntimeInitialized: wireControls,
};
