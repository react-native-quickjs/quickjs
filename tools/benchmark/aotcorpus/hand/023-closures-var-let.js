var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 23579;
var tot = 0;
for (var r = 0; r < N; r++) {
  var fv = [], fl = [];
  for (var i = 0; i < 5; i++) fv.push(function () { return i; });
  for (let j = 0; j < 5; j++) fl.push(function () { return j + r % 3; });
  for (var k = 0; k < 5; k++) { tot += fv[k]() * 10 + fl[k](); }
  var fi = [];
  for (var i2 = 0; i2 < 3; i2++) (function (q) { fi.push(function () { return q * 2; }); })(i2);
  tot += fi[0]() + fi[2]();
}
mix(tot);
print('closures-var-let ' + h);
