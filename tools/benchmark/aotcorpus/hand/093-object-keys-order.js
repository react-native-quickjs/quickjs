var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 20000;
var t = 0;
for (var i = 0; i < N; i++) {
  var o = { b: 1, 2: "x", a: 2, 1: "y", [-1]: 3, "01": 4 };
  o.c = 5; o[0] = 6;
  var ks = Object.keys(o);
  t += ks.join().length + ks.indexOf("a");
  if (i % 2000 == 0) mixs(ks.join(","));
  var s = 0; for (var k in o) s = s * 3 + k.length;
  t += s % 1000;
  delete o.b; o.b = 9;
  t += Object.keys(o).indexOf("b");
  t += JSON.stringify(o).length;
}
mix(t);
print('object-keys-order ' + h);
