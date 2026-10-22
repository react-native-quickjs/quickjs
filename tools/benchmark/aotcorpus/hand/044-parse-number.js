var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 55554;
var strs = ["42", " 17 ", "0x1f", "1e3", "3.14abc", "abc", "", "-0", "  -12.5e1x", "0b101", "1_000", ".5", "5.", "Infinity", "-Infinity"];
var tot = 0;
for (var i = 0; i < N; i++) {
  var s = strs[i % strs.length];
  mixd(parseInt(s)); mixd(parseInt(s, 16)); mixd(parseFloat(s)); mixd(Number(s)); mixd(+s);
  tot += isNaN(s) ? 1 : 0;
  tot += Number.isInteger(parseFloat(s)) ? 1 : 0;
}
mix(tot);
print('parse-number ' + h);
