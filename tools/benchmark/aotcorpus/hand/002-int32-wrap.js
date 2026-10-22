var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 432931;
var a = 1, b = 0x7fffffff, c = -5;
for (var i = 0; i < N; i++) {
  a = (a * 1664525 + 1013904223) | 0;
  b = (b + a) | 0;
  c = (c << 3) ^ (a >>> 7) ^ (b >> 3);
  mix(c);
}
mix(a); mix(b); mix(c);
print('int32-wrap ' + h);
