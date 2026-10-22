var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 568705;
var shapes = [];
for (var i = 0; i < 12; i++) {
  var o = {};
  o["p" + i] = i;
  o.v = i + 1;
  o["z" + (i % 4)] = 0;
  o.get = (function (k) { return function () { return this.v + k; }; })(i);
  shapes.push(o);
}
var tot = 0;
for (var i = 0; i < N; i++) {
  var o = shapes[i % 12];
  tot = (tot + o.get() + o.v) | 0;
}
mix(tot);
print('megamorphic-calls ' + h);
