var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 205333;
var tot = 0;
function g(i) {
  for (var j = 0; j < 3; j++) {
    try {
      if (j == i % 3) continue;
      if (i % 7 == 0 && j == 2) break;
      tot += j;
    } finally {
      tot += 10;
      if (i % 11 == 0 && j == 1) return -1;
    }
  }
  return j;
}
for (var i = 0; i < N; i++) tot += g(i);
mix(tot);
function hh(x) { try { try { throw x; } finally { tot++; } } catch (e) { return e * 2; } }
for (var i = 0; i < 1000; i++) tot += hh(i);
mix(tot);
print('finally-control-flow ' + h);
