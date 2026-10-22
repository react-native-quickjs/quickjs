var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 92;
var out = 0;
for (var py = 0; py < N; py++) for (var px = 0; px < N * 2; px++) {
  var x0 = px / N - 1.5, y0 = py / N * 2 - 1, x = 0, y = 0, it = 0;
  while (x * x + y * y <= 4 && it < 100) { var t = x * x - y * y + x0; y = 2 * x * y + y0; x = t; it++; }
  out = (out * 7 + it) | 0;
}
mix(out);
print('mandelbrot ' + h);
