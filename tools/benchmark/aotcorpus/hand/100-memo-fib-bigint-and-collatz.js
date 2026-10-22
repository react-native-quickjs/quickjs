var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 20000;
var best = 0, bi = 0;
for (var i = 1; i < N; i++) {
  var n = i, steps = 0;
  while (n != 1) { n = n % 2 == 0 ? n / 2 : 3 * n + 1; steps++; }
  if (steps > best) { best = steps; bi = i; }
}
mix(best); mix(bi);
var a = 0n, b = 1n;
for (var i = 0; i < 300; i++) { var c = a + b; a = b; b = c; }
mixs(a.toString().slice(0, 20));
print('memo-fib-bigint-and-collatz ' + h);
