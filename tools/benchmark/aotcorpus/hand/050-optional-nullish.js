var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 200821;
var data = [{ a: { b: { c: 1 } } }, { a: null }, {}, { a: { b: null } }, null, { a: { f: function () { return 5; } } }];
var tot = 0;
for (var i = 0; i < N; i++) {
  var d = data[i % 6];
  tot += d?.a?.b?.c ?? -1;
  tot += d?.a?.f?.() ?? -2;
  tot += d?.["a"]?.["b"] === undefined ? 1 : 0;
  var x = i % 4 == 0 ? 0 : i % 4 == 1 ? "" : i % 4 == 2 ? null : 3;
  tot += Number(x ?? 10) + Number(x || 20);
  var y = null; y ??= 7; var z = 0; z ||= 8; var w = 1; w &&= 9;
  tot += y + z + w;
}
mix(tot);
print('optional-nullish ' + h);
