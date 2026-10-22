var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 300;
var arr = [];
for (var i = 0; i < 200; i++) arr.push((i * 37) % 101);
function bubble(a) {
  var n = a.length;
  for (var i = 0; i < n; i++)
    for (var j = 0; j + 1 < n - i; j++)
      if (a[j] > a[j + 1]) [a[j], a[j + 1]] = [a[j + 1], a[j]];
  return a;
}
for (var r = 0; r < N / 10; r++) {
  var a = arr.slice();
  a[0] = r;
  bubble(a);
  mix(a[0]); mix(a[199]); mix(a[100]);
}
print('array-destructuring-swap ' + h);
