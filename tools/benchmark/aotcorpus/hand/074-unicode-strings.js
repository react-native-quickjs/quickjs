var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 14149;
var t = 0;
for (var i = 0; i < N; i++) {
  var s = "aé中😀z" + i;
  t += s.length + [...s].length + s.codePointAt(3) + s.charCodeAt(4) + s.at(3).length;
  t += String.fromCodePoint(0x1f600 + i % 10).length + s.indexOf("\ud83d");
  t += encodeURIComponent(s).length + decodeURIComponent(encodeURIComponent(s)).length;
  t += s.toUpperCase().length + escape("é ").length;
  t += s.split("").reverse().join("").length + [...s].reverse().join("").charCodeAt(0);
  t += JSON.stringify(s).length;
}
mix(t);
print('unicode-strings ' + h);
