var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 115;
var pts = [];
for (var i = 0; i < 1000; i++) pts.push({ x: i, y: i * 2, name: "p" + i, tags: [i % 3, i % 5] });
var t = 0;
for (var r = 0; r < N; r++) {
  var s = pts.filter(function (p) { return p.tags[0] == r % 3; }).map(function (p) { return p.x + p.y; }).reduce(function (a, b) { return a + b; }, 0);
  t = (t + s) | 0;
  pts.sort(function (a, b) { return (a.tags[1] - b.tags[1]) || (a.x - b.x); });
  t ^= pts[r % 1000].x;
}
mix(t);
print('array-of-objects-sum ' + h);
