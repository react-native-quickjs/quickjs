// Differential corpus for the static top-of-stack cache and suspension.
//
// WHY THIS FILE.  A generator or async function suspends in the middle of an
// expression, and the frame is torn down and rebuilt around the suspension
// point.  A value the cache was holding has no memory slot to be saved into, so
// the compiler must have spilled it before the suspension.  The printed results
// are what the caller sees across the resume.

function show(k, v) { print(k + '=' + v); }

function* genArith(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t = t + i * 3;
    yield t;
  }
  return t + 100;
}
var g = genArith(4);
var out = [];
var step;
while (!(step = g.next()).done) out.push(step.value);
show('genArith', out.join(','));
show('genArithReturn', g.next().value);

function* genAcrossYield(a, b) {
  var x = a + b;
  yield x;
  var y = x * 2;
  yield y;
  return x + y;
}
var g2 = genAcrossYield(3, 4);
show('g2a', g2.next().value);
show('g2b', g2.next().value);
show('g2c', g2.next().value);

function* genTryCatch() {
  try {
    yield 1;
    throw new Error('thrown');
  } catch (e) {
    yield 'caught:' + e.message;
  }
  return 'done';
}
var g3 = genTryCatch();
show('g3a', g3.next().value);
show('g3b', g3.next().value);
show('g3c', g3.next().value);

function* genDelegate(n) {
  var t = 0;
  for (var i = 0; i < n; i++) t = t + i;
  yield* [t, t * 2];
  return t;
}
var g4 = genDelegate(4);
show('g4a', g4.next().value);
show('g4b', g4.next().value);
show('g4c', g4.next().value);

// A generator that is suspended while a cached value is live, then resumed and
// used in a further computation: the value has to be spilled at the yield.
function* genSuspendedValue(a, b) {
  var t = a * b;
  yield t;
  var u = t + a;
  yield u;
  return u * b;
}
var g5 = genSuspendedValue(3, 5);
show('g5a', g5.next().value);
show('g5b', g5.next().value);
show('g5c', g5.next().value);

// Two generators interleaved, so each resume rebuilds a frame while the other
// is suspended with a live cache.
function* genPair(seed) {
  var t = seed;
  yield t;
  t = t + 1;
  yield t;
  return t * 2;
}
var pa = genPair(10), pb = genPair(100);
show('pair0', pa.next().value + ',' + pb.next().value);
show('pair1', pa.next().value + ',' + pb.next().value);
show('pair2', pa.next().value + ',' + pb.next().value);
