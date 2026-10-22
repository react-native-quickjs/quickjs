var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3961;
var tot = 0;
for (var r = 0; r < N; r++) {
  outer:
  for (var i = 0; i < 40; i++) {
    inner:
    for (var j = 0; j < 40; j++) {
      if ((i * j + r) % 11 == 0) continue outer;
      if (j > i) continue inner;
      if (i + j > 70) break outer;
      tot += i ^ j;
    }
  }
  blk: { if (r % 2) break blk; tot += 3; }
}
mix(tot);
print('labeled-loops ' + h);
