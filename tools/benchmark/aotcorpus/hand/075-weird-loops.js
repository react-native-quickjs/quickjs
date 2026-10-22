var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 1829616;
var t = 0;
var i = 0;
do { t += i; i += 3; } while (i < N);
while (true) { if (--i < N / 2) break; t ^= i; }
for (var a = 0, b = N; a < b; a += 2, b -= 3) t = (t + a * b) | 0;
for (;;) { t = (t + 1) | 0; if (t % 5 == 0) break; }
var k = 0;
for (k in { a: 1, b: 2, c: 3 }) t += k.charCodeAt(0);
var j = 10;
while (j --> 0) t += j;
for (var x of [1, 2, 3]) { if (x == 2) continue; t += x; }
mix(t);
print('weird-loops ' + h);
