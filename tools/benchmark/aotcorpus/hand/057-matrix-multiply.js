var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 21;
var M = 40;
function mk(n, f) { var m = []; for (var i = 0; i < n; i++) { m[i] = []; for (var j = 0; j < n; j++) m[i][j] = f(i, j); } return m; }
var A = mk(M, function (i, j) { return (i * 3 + j) % 10 / 4; });
var B = mk(M, function (i, j) { return (i + j * 7) % 13 - 6; });
for (var rep = 0; rep < N; rep++) {
  var C = mk(M, function () { return 0; });
  for (var i = 0; i < M; i++)
    for (var j = 0; j < M; j++) {
      var s = 0;
      for (var k = 0; k < M; k++) s += A[i][k] * B[k][j];
      C[i][j] = s;
    }
  A[rep % M][rep % M] += 1;
  var tr = 0;
  for (var i = 0; i < M; i++) { tr += C[i][i]; mixd(C[i][(i * 5) % M]); }
  mixd(tr);
}
print('matrix-multiply ' + h);
