var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 469790;
var a = [];
for (var i = 0; i < N; i++) a[i] = i * 2;
mix(a.length);
var b = [];
for (var i = N; i > 0; i -= 2) b[i] = i;
mix(b.length);
var t = 0;
for (var i = 0; i < b.length; i++) if (b[i] !== undefined) t += b[i] & 255;
mix(t);
var c = [];
c[100] = 1; c.length = 50; c[60] = 2;
mix(c.length);
var d = new Array(1000);
for (var i = 0; i < d.length; i++) d[i] = i;
mix(d.reduce(function (p, x) { return p + x; }));
print('array-growth-index ' + h);
