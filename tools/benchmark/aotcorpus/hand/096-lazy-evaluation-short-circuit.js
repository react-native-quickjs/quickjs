var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 150000;
var calls = 0;
function f(x) { calls++; return x; }
var t = 0;
for (var i = 0; i < N; i++) {
  t += (f(i & 1) && f(2) || f(3)) + (f(0) || f(i & 3) || f(9)) + (f(null) ?? f(4));
  t += (i & 1 ? f(1) : f(0)) && 5;
  if (f(i % 5 == 0) && f(i % 3 == 0)) t++;
}
mix(t); mix(calls);
print('lazy-evaluation-short-circuit ' + h);
