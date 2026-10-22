var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 3043;
var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function enc(bytes) {
  var s = "";
  for (var i = 0; i < bytes.length; i += 3) {
    var n = (bytes[i] << 16) | ((bytes[i + 1] || 0) << 8) | (bytes[i + 2] || 0);
    s += chars[n >> 18] + chars[(n >> 12) & 63] + (i + 1 < bytes.length ? chars[(n >> 6) & 63] : "=") + (i + 2 < bytes.length ? chars[n & 63] : "=");
  }
  return s;
}
function dec(s) {
  var out = [];
  for (var i = 0; i < s.length; i += 4) {
    var n = 0, pad = 0;
    for (var j = 0; j < 4; j++) { var c = s[i + j]; if (c == "=") { pad++; n <<= 6; } else n = (n << 6) | chars.indexOf(c); }
    out.push((n >> 16) & 255); if (pad < 2) out.push((n >> 8) & 255); if (pad < 1) out.push(n & 255);
  }
  return out;
}
for (var r = 0; r < N; r++) {
  var b = []; for (var i = 0; i < 50 + r % 3; i++) b.push((i * 31 + r) & 255);
  var e = enc(b), d = dec(e);
  mixs(e.slice(0, 8)); mix(d.length); mix(d.join() == b.join() ? 1 : 0);
}
print('base64 ' + h);
