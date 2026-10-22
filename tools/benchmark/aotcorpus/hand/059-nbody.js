var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 51684;
var bodies = [];
for (var i = 0; i < 5; i++) bodies.push({ x: i * 1.5, y: i * i * 0.3 - 1, z: 0.1 * i, vx: 0.01 * i, vy: -0.02 * i, vz: 0.005, m: 1 + i * 0.5 });
function step(dt) {
  for (var i = 0; i < 5; i++) {
    var a = bodies[i];
    for (var j = i + 1; j < 5; j++) {
      var b = bodies[j];
      var dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
      var d2 = dx * dx + dy * dy + dz * dz + 0.01;
      var mag = dt / (d2 * Math.sqrt(d2));
      a.vx -= dx * b.m * mag; a.vy -= dy * b.m * mag; a.vz -= dz * b.m * mag;
      b.vx += dx * a.m * mag; b.vy += dy * a.m * mag; b.vz += dz * a.m * mag;
    }
  }
  for (var i = 0; i < 5; i++) { var a = bodies[i]; a.x += dt * a.vx; a.y += dt * a.vy; a.z += dt * a.vz; }
}
for (var i = 0; i < N; i++) step(0.01);
for (var i = 0; i < 5; i++) { mixd(bodies[i].x); mixd(bodies[i].y); mixd(bodies[i].vz); }
print('nbody ' + h);
