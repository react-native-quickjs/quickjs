var __SINK__ = 0;

function __blackhole__(v) {
  if (v === undefined || v === null) { __SINK__++; return; }
  var t = typeof v;
  if (t === 'number') __SINK__ += v | 0;
  else if (t === 'string') __SINK__ += v.length;
  else if (t === 'boolean') __SINK__ += v ? 1 : 0;
  else if (t === 'object') __SINK__ += v.length === undefined ? 1 : v.length | 0;
  else __SINK__++;
}

var __RESULTS__ = [];

function bench(spec) {
  var minMs = spec.minMs || 100;
  var reps = spec.reps || 7;
  var unit = spec.unit || 'op';

  if (spec.setup) spec.setup();

  if (spec.expect !== undefined) {
    var got = spec.run();
    var want = typeof spec.expect === 'function' ? spec.expect(got) : spec.expect;
    var ok = typeof spec.expect === 'function' ? want === true : got === want;
    if (!ok) {
      __RESULTS__.push({ name: spec.name, error: 'expected ' + want + ', got ' + got });
      return;
    }
  }

  function timeIters(fn, iters) {
    var t0 = Date.now();
    for (var i = 0; i < iters; i++) __blackhole__(fn());
    return Date.now() - t0;
  }

  var pinned =
    typeof __BENCH_PINNED_ITERS__ !== 'undefined' && __BENCH_PINNED_ITERS__
      ? __BENCH_PINNED_ITERS__[spec.name]
      : undefined;

  var samples;
  var iters;
  if (pinned) {
    iters = pinned;
    samples = [];
    for (var pr = 0; pr < reps; pr++) {
      if (spec.before) spec.before();
      samples.push(timeIters(spec.run, iters));
    }
  } else {
    iters = 1;
    var elapsed;
    do { iters *= 2; elapsed = timeIters(spec.run, iters); } while (elapsed < minMs);
    samples = [elapsed];
    for (var r = 1; r < reps; r++) {
      if (spec.before) spec.before();
      samples.push(timeIters(spec.run, iters));
    }
  }

  samples.sort(function (a, b) { return a - b; });
  var minNs = (samples[0] * 1e6) / iters;
  var medNs = (samples[(samples.length - 1) >> 1] * 1e6) / iters;
  var maxNs = (samples[samples.length - 1] * 1e6) / iters;
  var spreadPct =
    samples[0] > 0 ? ((samples[samples.length - 1] - samples[0]) / samples[0]) * 100 : 0;

  __RESULTS__.push({
    name: spec.name, unit: unit, iters: iters, reps: reps,
    pinned: !!pinned, minNs: minNs, medNs: medNs, maxNs: maxNs, spreadPct: spreadPct
  });
}

function __BENCH_REPORT__() {
  for (var i = 0; i < __RESULTS__.length; i++)
    print('##BENCH## ' + JSON.stringify(__RESULTS__[i]));
}