var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 833;
var n = 500;
var px = new Float64Array(n), py = new Float64Array(n), vx = new Float32Array(n), vy = new Float32Array(n);
for (var i = 0; i < n; i++) { px[i] = i; py[i] = n - i; vx[i] = (i % 7) - 3; vy[i] = (i % 5) - 2; }
for (var s = 0; s < N; s++) {
  for (var i = 0; i < n; i++) {
    px[i] += vx[i] * 0.1; py[i] += vy[i] * 0.1;
    if (px[i] < 0 || px[i] > n) vx[i] = -vx[i];
    if (py[i] < 0 || py[i] > n) vy[i] = -vy[i];
  }
}
var sx = 0, sy = 0; for (var i = 0; i < n; i++) { sx += px[i]; sy += py[i]; }
mixd(sx); mixd(sy);
print('struct-of-arrays-physics ' + h);
