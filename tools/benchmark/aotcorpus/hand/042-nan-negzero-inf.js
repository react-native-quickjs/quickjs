var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 54229;
var tot = 0;
var specials = [0, -0, NaN, Infinity, -Infinity, 1, -1, 0.5];
for (var i = 0; i < N; i++) {
  var a = specials[i % 8], b = specials[(i * 3 + 1) % 8];
  var r = [a + b, a - b, a * b, a / b, a % b, Math.min(a, b), Math.max(a, b), a < b, a >= b];
  for (var k = 0; k < r.length; k++) {
    var x = r[k];
    tot += x !== x ? 1 : x === Infinity ? 2 : x === -Infinity ? 3 : (1 / x === -Infinity) ? 4 : typeof x == "boolean" ? (x ? 5 : 6) : 7;
  }
}
mix(tot);
for (var i = 0; i < 8; i++) mixs(String(1 / specials[i]) + String(Math.sign(specials[i])) + Object.is(Math.round(specials[i] - 0.5), -0));
print('nan-negzero-inf ' + h);
