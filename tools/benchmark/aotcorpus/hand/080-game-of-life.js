var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 40;
var W = 48, H = 48;
var g = new Uint8Array(W * H);
for (var i = 0; i < W * H; i++) g[i] = ((i * 2654435761) >>> 28) < 5 ? 1 : 0;
for (var gen = 0; gen < N; gen++) {
  var n = new Uint8Array(W * H);
  for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
    var c = 0;
    for (var dy = -1; dy <= 1; dy++) for (var dx = -1; dx <= 1; dx++) if (dx || dy) c += g[((y + dy + H) % H) * W + (x + dx + W) % W];
    n[y * W + x] = c == 3 || (c == 2 && g[y * W + x]) ? 1 : 0;
  }
  g = n;
  var alive = 0; for (var i = 0; i < g.length; i++) alive += g[i];
  mix(alive);
}
print('game-of-life ' + h);
