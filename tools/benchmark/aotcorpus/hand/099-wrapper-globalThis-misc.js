var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 50000;
var t = 0;
var o = { n: 0 };
for (var i = 0; i < N; i++) {
  globalThis["g" + (i % 8)] = i;
  t += globalThis["g" + ((i + 3) % 8)] | 0;
  o.n += Number(String(i).length);
  t += [i, "a"].toString().length + Boolean(i & 1) + (+(i % 2 == 0));
  t += parseInt("12" + (i % 10)) + Number.MAX_SAFE_INTEGER % 1000 + Number.EPSILON * 0;
  t = t % 1000003;
}
mix(t); mix(o.n);
print('wrapper-globalThis-misc ' + h);
