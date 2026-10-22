var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 16894;
var tot = 0;
for (var i = 0; i < N; i++) {
  var a = { x: i, y: 2 };
  var b = { y: 3, z: 4 };
  var c = Object.assign({}, a, b, { w: 0 });
  var d = { ...a, ...b, x: -1 };
  var { x, ...rest } = c;
  tot += c.x + c.y + c.z + d.x + d.y + x + Object.keys(rest).length;
  var es = Object.entries(d);
  tot += es.length;
  for (var [k, v] of es) tot += k.length + v;
  var o2 = Object.fromEntries(es.map(function (e) { return [e[0] + "!", e[1]]; }));
  tot += o2["x!"];
  tot += Object.values(c).length;
}
mix(tot);
print('object-assign-spread ' + h);
