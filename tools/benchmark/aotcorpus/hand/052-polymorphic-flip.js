var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 208168;
function get(o) { return o.x + 1; }
var objs = [{ x: 1 }, { y: 0, x: 2 }, { x: 3.5 }, { x: "s" }, { x: 4n }];
var tot = 0;
for (var i = 0; i < N; i++) {
  var o = i < N / 2 ? objs[i & 1] : objs[(i % 3) ? (i & 1) : 2 + (i % 2)];
  var r = get(o);
  tot += typeof r == "bigint" ? 5 : typeof r == "string" ? r.length : r;
  tot = tot % 1e9;
}
mixd(tot);
function add(a, b) { return a + b; }
var vals = [1, 2.5, "a", 2, 3n, null, true];
var s = 0;
for (var i = 0; i < N / 2; i++) {
  var x = i < 20000 ? i : vals[i % 7];
  var y = i < 30000 ? 1 : vals[(i + 3) % 7];
  try { var r = add(x, y); s += typeof r == "number" ? r : String(r).length; } catch (e) { s += 1; }
}
mixd(s);
print('polymorphic-flip ' + h);
