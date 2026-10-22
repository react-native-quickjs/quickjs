var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 172324;
var vals = [1, "s", true, null, undefined, {}, [], function () {}, Symbol("x"), 10n, new Date(0), /r/, NaN];
var tot = 0;
for (var i = 0; i < N; i++) {
  var v = vals[i % vals.length];
  var t = typeof v;
  tot += t.length;
  tot += (v instanceof Object ? 1 : 0) + (v instanceof Array ? 2 : 0) + (v instanceof Function ? 4 : 0);
  tot += ("length" in Object(v)) ? 1 : 0;
  if (i < 13) mixs(t);
}
mix(tot);
print('typeof-instanceof-in ' + h);
