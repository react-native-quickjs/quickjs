var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 126839;
function sum() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s; }
function sloppy(a, b) { arguments[0] = 100; b = 7; return a + arguments[1] + arguments.length; }
function pass() { return sum.apply(null, arguments) + sum.apply(this, [].slice.call(arguments, 1)); }
var tot = 0;
for (var i = 0; i < N; i++) {
  tot = (tot + sum(i, 1, 2) + sloppy(i, 2) + pass(i, i + 1, 3) + sum()) | 0;
}
mix(tot);
print('arguments-object ' + h);
