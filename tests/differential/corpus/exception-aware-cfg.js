// Differential corpus for the exception-aware CFG.
//
// WHY THIS FILE.  A `catch` gives control a route the ordinary block graph does
// not show: any instruction between a `catch` and the point its offset is
// dropped can transfer to the handler, with the region's stack thrown away and
// the exception left where the offset was. The pass reads such a function now,
// so what it may not do is assume a local across that route. Every row here is
// one of the ways it could: the handler writing what the region wrote, a value
// computed before the `catch` and read after it, a nested handler, a rethrow
// out of one, and the path that returns through the region. The file is run
// against node as well as `qjsc -O`.

function show(k, v) { print(k + '=' + v); }

// The handler writes the local the region wrote, and it is read after both.
function merge(flag) {
  var x = 1, y = 0;
  try { if (flag) throw 2; x = 3; y = 5; } catch (e) { x = 4; }
  return x * 10 + y;
}
for (var i = 0; i < 4; i++) show('merge' + i, merge((i & 1) === 1));

// A value computed before the `catch` is still there after the region.
function keep(n) {
  var t = 0;
  for (var j = 0; j < n; j++) {
    var before = j * 2;
    try { if (j === 3) throw j; t += 1; } catch (e) { t -= 1; }
    t += before;
  }
  return t;
}
show('keep', keep(8));

// A handler inside a handler, and a rethrow out of the inner one.
function nest(n) {
  var t = 0;
  for (var k = 0; k < n; k++) {
    try {
      try { if (k & 1) throw k; t += 1; }
      catch (e) { if (e > 4) throw e; t += 2; }
    } catch (e2) { t += 4; }
  }
  return t;
}
show('nest', nest(10));

// The region's own value, on the path that did not throw and on the one that did.
function fall(g) {
  var r = 'a';
  try { r = g(); } catch (e) { r = 'threw'; }
  return r;
}
show('fall1', fall(function () { return 'b'; }));
show('fall2', fall(function () { throw 1; }));

// A constant body and a dead store inside the region.
function fold(n) {
  var t = 0;
  for (var m = 0; m < n; m++) {
    try { var dead = m * 3; t += 1 + 2; } catch (e) { t = -1; }
  }
  return t;
}
show('fold', fold(10));
