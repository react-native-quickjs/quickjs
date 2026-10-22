var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 63160;
var vals = [0, -0, 1, "1", "", " ", null, undefined, NaN, false, true, [], [0], [1], {}, "0", "a", Infinity, 0n, 1n];
var tot = 0;
for (var r = 0; r < N / 100 + 1; r++)
  for (var i = 0; i < vals.length; i++)
    for (var j = 0; j < vals.length; j++) {
      var e = vals[i] == vals[j], s = vals[i] === vals[j];
      tot += (e ? 1 : 0) + (s ? 2 : 0) + (Object.is(vals[i], vals[j]) ? 4 : 0);
      if (r == 0) mix((e ? 1 : 0) + (s ? 2 : 0));
    }
mix(tot);
print('equality-corners ' + h);
