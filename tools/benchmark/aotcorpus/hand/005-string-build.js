var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 121153;
var s = "";
for (var i = 0; i < N; i++) {
  s += String.fromCharCode(97 + i % 26);
  if (i % 7 == 0) s += i;
  if (i % 100 == 0) s += "-";
}
mix(s.length);
var cnt = 0;
for (var i = 0; i < s.length; i++) cnt += s.charCodeAt(i) & 15;
mix(cnt);
mixs(s.slice(0, 50));
mix(s.indexOf("xyz")); mix(s.lastIndexOf("abc"));
print('string-build ' + h);
