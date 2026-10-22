var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 116615;
var t = 0;
for (var i = 1; i <= N; i++) {
  var x = i / 100;
  t += Math.sqrt(x) + Math.sin(x) * Math.cos(x) + Math.atan2(x, 3) + Math.log(x) + Math.exp(x % 5) + Math.pow(x, 0.3) + Math.abs(-x) + Math.floor(-x) + Math.ceil(x) + Math.round(x - 0.5) + Math.trunc(-x) + Math.hypot(x, 4) + Math.cbrt(x) + Math.sign(x - 150) + Math.fround(x) + Math.clz32(i) + Math.imul(i, 65537) + Math.log2(x) + Math.log10(x) + Math.tanh(x % 3) + Math.atan(x) + Math.expm1(x % 2) + Math.log1p(x);
  if (i % 3000 == 0) mixd(t);
}
mixd(t);
print('math-functions ' + h);
