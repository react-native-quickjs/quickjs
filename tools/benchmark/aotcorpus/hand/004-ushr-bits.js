var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 200000;
function popcnt(v) { v = v - ((v >>> 1) & 0x55555555); v = (v & 0x33333333) + ((v >>> 2) & 0x33333333); return (((v + (v >>> 4)) & 0x0f0f0f0f) * 0x01010101) >>> 24; }
function rev(v) { var r = 0; for (var i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>>= 1; } return r; }
var s = 99, t = 0;
for (var i = 0; i < N; i++) {
  s = (s ^ (s << 13)) | 0; s = (s ^ (s >>> 17)) | 0; s = (s ^ (s << 5)) | 0;
  t += popcnt(s);
  if (i % 16 == 0) mix(rev(s));
  mix(s >>> 0 > 0x7fffffff ? 1 : 0);
}
mix(t);
print('ushr-bits ' + h);
