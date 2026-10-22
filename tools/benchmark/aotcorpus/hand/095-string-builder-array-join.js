var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 8154;
var tot = 0;
for (var r = 0; r < N; r++) {
  var parts = [];
  for (var i = 0; i < 30; i++) parts.push(i % 2 ? "x" + i : i * r);
  var s = parts.join("");
  tot += s.length + s.charCodeAt(s.length - 1);
  var t = "";
  for (var i = 0; i < 20; i++) t = i % 2 ? t + i : i + t;
  tot += t.length + t.charCodeAt(3);
  tot += (r + "|" + (r / 4) + "|" + (r % 3 == 0) + "|" + null + "|" + undefined + "|" + [r, r]).length;
}
mix(tot);
print('string-builder-array-join ' + h);
