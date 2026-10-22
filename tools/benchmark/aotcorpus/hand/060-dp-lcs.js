var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 25;
var seed = 11;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 6; }
function rs(n) { var s = ""; for (var i = 0; i < n; i++) s += "abcd"[rnd() & 3]; return s; }
function lcs(a, b) {
  var dp = [];
  for (var i = 0; i <= a.length; i++) { dp.push(new Array(b.length + 1).fill(0)); }
  for (var i = 1; i <= a.length; i++)
    for (var j = 1; j <= b.length; j++)
      dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1 : Math.max(dp[i - 1][j], dp[i][j - 1]);
  return dp[a.length][b.length];
}
function edit(a, b) {
  var prev = [], cur = [];
  for (var j = 0; j <= b.length; j++) prev[j] = j;
  for (var i = 1; i <= a.length; i++) {
    cur = [i];
    for (var j = 1; j <= b.length; j++) cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a.charCodeAt(i - 1) != b.charCodeAt(j - 1) ? 1 : 0));
    prev = cur;
  }
  return prev[b.length];
}
for (var i = 0; i < N; i++) { var x = rs(120), y = rs(110); mix(lcs(x, y)); mix(edit(x, y)); }
print('dp-lcs ' + h);
