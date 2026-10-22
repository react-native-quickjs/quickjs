var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 346;
var seed = 31;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 5; }
var ws = [];
for (var i = 0; i < 300; i++) { var s = ""; var l = 1 + rnd() % 6; for (var j = 0; j < l; j++) s += String.fromCharCode(65 + rnd() % 26 + (rnd() & 1) * 32); ws.push(s); }
for (var r = 0; r < N; r++) {
  var a = ws.slice();
  a.sort(function (x, y) { return x < y ? -1 : x > y ? 1 : 0; });
  mixs(a[r % 300]); mixs(a[0]);
  var b = ws.slice(r % 50).sort(function (x, y) { return x.length - y.length || (x < y ? 1 : -1); });
  mixs(b[0]);
  mix(a.indexOf("A")); 
}
print('string-compare-sort ' + h);
