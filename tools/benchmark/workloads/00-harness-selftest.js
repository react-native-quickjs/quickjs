/*
 * Harness self-test.
 *
 * Not a performance workload — it exists to prove the harness itself is sound
 * on every engine before any real number is trusted. It checks the three things
 * that have actually gone wrong in benchmark harnesses on this project:
 *
 *   1. the timer resolves and calibration reaches minMs
 *   2. `expect` catches a wrong result, rather than timing garbage
 *   3. the benchmark body is NOT dead-code-eliminated
 *
 * (3) is the one that matters most: Hermes runs a real optimizer at -O and will
 * delete a loop whose result is unused. If `dce-canary` ever reports a
 * suspiciously tiny per-op cost on one engine and not the other, the harness is
 * lying and every other number in the suite is suspect.
 */

// 1. Calibration and timing.
bench({
  name: 'selftest/arith',
  unit: 'iteration',
  run: function () {
    var s = 0;
    for (var i = 0; i < 1000; i++) s += i * 3;
    return s;
  },
  expect: 1498500,
});

// 2. The canary. This allocates and returns a value that depends on every
//    iteration, so no optimizer may remove the loop. A per-op cost far below
//    what the work implies means elimination happened anyway.
bench({
  name: 'selftest/dce-canary',
  unit: 'iteration',
  run: function () {
    var a = [];
    for (var i = 0; i < 256; i++) a.push(i ^ (i << 3));
    var t = 0;
    for (var j = 0; j < a.length; j++) t += a[j];
    return t;
  },
  expect: 262016,
});

// 3. String work, which uses a different allocator path than the numeric cases
//    and catches a harness that only exercises one.
bench({
  name: 'selftest/string',
  unit: 'iteration',
  run: function () {
    var s = '';
    for (var i = 0; i < 100; i++) s += 'x';
    return s.length;
  },
  expect: 100,
});
