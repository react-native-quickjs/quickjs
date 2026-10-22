var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 4000;
var t = 0;
for (var i = 0; i < N; i++) {
  var o = { a: [1, "two", null, undefined, function () {}, NaN, -0, Infinity], b: undefined, c: { toJSON: function () { return "tj" + i; } }, d: new Date(86400000 * i), e: " \n\t\"\\", f: 1e21, g: 1.5e-7, [Symbol("s")]: 1 };
  var s = JSON.stringify(o);
  t += s.length;
  if (i % 400 == 0) mixs(s);
  t += JSON.stringify(o, ["a", "c"]).length + JSON.stringify(o, function (k, v) { return typeof v == "number" ? v + 1 : v; }).length;
  t += JSON.stringify([1, [2, { x: 3 }]], null, 2).length + JSON.stringify({ q: 1 }, null, "--").length;
  t += JSON.parse("[1,2,{\"a\":" + i + "}]", function (k, v) { return typeof v == "number" ? v * 2 : v; })[2].a;
  t += JSON.stringify("\ud800").length;
}
mix(t);
print('json-edge ' + h);
