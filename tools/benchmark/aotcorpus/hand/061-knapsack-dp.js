var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 92;
var w = [], v = [];
for (var i = 0; i < 60; i++) { w.push(3 + (i * 7) % 23); v.push(10 + (i * 13) % 37); }
for (var r = 0; r < N; r++) {
  var cap = 400 + r;
  var dp = new Int32Array(cap + 1);
  for (var i = 0; i < w.length; i++)
    for (var c = cap; c >= w[i]; c--) { var t = dp[c - w[i]] + v[i]; if (t > dp[c]) dp[c] = t; }
  mix(dp[cap]);
}
var coins = [1, 5, 10, 25, 50], ways = new Array(5001).fill(0); ways[0] = 1;
for (var i = 0; i < coins.length; i++) for (var s = coins[i]; s <= 5000; s++) ways[s] += ways[s - coins[i]];
mixd(ways[5000]);
print('knapsack-dp ' + h);
