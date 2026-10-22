var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 50206;
function tag(strs, ...vals) { var r = ""; for (var i = 0; i < strs.length; i++) r += strs[i] + (i < vals.length ? "[" + vals[i] + "]" : ""); return r; }
function raw(s) { return s.raw[0].length + s[0].length; }
var tot = 0;
for (var i = 0; i < N; i++) {
  var s = tag`a${i}b${i * 2}c`;
  tot += s.length + raw`x\n${1}`.valueOf() ;
  tot += `${i}-${i % 3 ? "odd" : "even"}`.length;
  tot += String.raw`a\tb${i}`.length;
}
mix(tot);
function same(s) { return s; }
mix(same`q` === same`q` ? 1 : 0);
var f = function () { return same`q`; };
mix(f() === f() ? 1 : 0);
print('tagged-templates ' + h);
