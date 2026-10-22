var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 920979;
var x = 1, y = 2147483647, z = 0;
for (var i = 0; i < N; i++) {
  y = y + 1;
  if (y > 2147483700) y = y - 100;
  x = x * 1.0001;
  if (x > 1e6) x = x / 1e6;
  z = z + (y * 3) % 7 + (x | 0);
  if (i % 1000 == 0) mixd(x);
}
mixd(x); mix(y); mixd(z);
print('double-overflow ' + h);
