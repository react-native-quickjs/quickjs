var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 60;
var W = 40, H = 40;
function bfs(sx, sy, walls) {
  var dist = new Int32Array(W * H).fill(-1), q = [sx + sy * W];
  dist[q[0]] = 0;
  for (var qi = 0; qi < q.length; qi++) {
    var cur = q[qi], x = cur % W, y = (cur / W) | 0;
    var d = [[1, 0], [-1, 0], [0, 1], [0, -1]];
    for (var k = 0; k < 4; k++) {
      var nx = x + d[k][0], ny = y + d[k][1];
      if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
      var id = nx + ny * W;
      if (dist[id] >= 0 || walls[id]) continue;
      dist[id] = dist[cur] + 1; q.push(id);
    }
  }
  return dist;
}
for (var r = 0; r < N; r++) {
  var walls = new Uint8Array(W * H);
  for (var i = 0; i < W * H; i++) walls[i] = ((i * 7 + r * 13) % 11 == 0 && i % W != 0) ? 1 : 0;
  var dist = bfs(0, 0, walls), mx = 0, sm = 0;
  for (var i = 0; i < dist.length; i++) { if (dist[i] > mx) mx = dist[i]; sm += dist[i]; }
  mix(mx); mix(sm);
}
print('queue-bfs-grid ' + h);
