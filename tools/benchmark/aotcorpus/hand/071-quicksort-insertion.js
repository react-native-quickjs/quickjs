var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 40;
var seed = 77;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed >> 3; }
function qs(a, lo, hi) {
  while (lo < hi) {
    var p = a[(lo + hi) >> 1], i = lo, j = hi;
    while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { var t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
    if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
  }
}
function ins(a) { for (var i = 1; i < a.length; i++) { var x = a[i], j = i - 1; while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; } a[j + 1] = x; } }
for (var r = 0; r < N; r++) {
  var a = []; for (var i = 0; i < 5000; i++) a.push(rnd() % 100000);
  var b = a.slice(0, 300);
  qs(a, 0, a.length - 1); ins(b);
  mix(a[2500]); mix(b[150]); mix(a[0]);
}
print('quicksort-insertion ' + h);
