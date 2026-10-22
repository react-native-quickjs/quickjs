var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 6460;
function* range(a, b) { for (var i = a; i < b; i++) yield i; }
function* take(g, n) { var c = 0; for (var v of g) { if (c++ >= n) return; yield v; } }
function* fibg() { var a = 0, b = 1; for (;;) { yield a; var t = a + b; a = b; b = t; } }
function* inner() { var x = yield 1; yield x * 2; return 99; }
function* outer() { var r = yield* inner(); yield r; }
var tot = 0;
for (var i = 0; i < N; i++) {
  for (var v of take(fibg(), 15)) tot += v;
  var s = 0; for (var v of range(0, 20)) s += v;
  tot += s;
  var g = outer(); g.next(); tot += g.next(i).value + g.next().value;
  tot += [...take(range(0, 100), 5)].length;
}
mix(tot);
print('generators ' + h);
