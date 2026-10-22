var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 2000;
var seed = 12345;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; }
function mk(d) {
  if (d == 0) return rnd() % 3 == 0 ? null : (rnd() % 2 ? rnd() % 1000 : "s" + (rnd() % 50));
  var o = {};
  for (var i = 0; i < 3; i++) o["k" + i] = mk(d - 1);
  o.arr = [d, d * 2.5, "x\n\"" + d, true];
  return o;
}
for (var i = 0; i < N; i++) {
  var v = mk(2);
  var s = JSON.stringify(v);
  var w = JSON.parse(s);
  mixs(s);
  mixs(JSON.stringify(w) === s ? "same" : "diff");
}
print('json-roundtrip ' + h);
