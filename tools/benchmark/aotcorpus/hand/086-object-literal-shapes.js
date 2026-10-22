var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 46442;
var t = 0;
for (var i = 0; i < N; i++) {
  var k = "dyn" + (i % 4);
  var o = { a: i, [k]: 2, b: "s", get g() { return this.a; }, m() { return 3; }, ["c" + (i % 2)]: 1, ...(i % 3 ? { x: 1 } : null) };
  t += o.a + o[k] + o.g + o.m() + (o.c0 || 0) + (o.c1 || 0) + (o.x || 0);
  t = t % 1000003;
}
mix(t);
print('object-literal-shapes ' + h);
