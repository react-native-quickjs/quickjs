var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 65245;
var tot = 0;
for (var i = 0; i < N; i++) {
  var o = { a: 1, b: 2 };
  o["c" + (i % 5)] = i;
  o.d = o.a + o.b;
  delete o.a;
  o.a = 10;
  var ks = Object.keys(o);
  tot += ks.length;
  if (i % 100 == 0) mixs(ks.join());
  tot += o.b + (o.c0 || 0) + ("c1" in o ? 1 : 0) + (o.hasOwnProperty("zz") ? 1 : 0);
}
mix(tot);
print('object-props ' + h);
