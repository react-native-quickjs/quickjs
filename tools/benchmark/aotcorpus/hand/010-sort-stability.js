var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 1319;
var seed = 3;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 4; }
for (var i = 0; i < N; i++) {
  var a = [];
  for (var j = 0; j < 80; j++) a.push({ k: rnd() % 5, id: j });
  a.sort(function (p, q) { return p.k - q.k; });
  var ok = 1;
  for (var j = 1; j < a.length; j++) if (a[j - 1].k == a[j].k && a[j - 1].id > a[j].id) ok = 0;
  mix(ok); mix(a[0].id); mix(a[79].id);
}
print('sort-stability ' + h);
