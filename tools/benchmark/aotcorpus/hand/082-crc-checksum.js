var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 550150;
var tab = new Int32Array(256);
for (var i = 0; i < 256; i++) { var c = i; for (var k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xEDB88320 : c >>> 1; tab[i] = c; }
var data = new Uint8Array(N);
for (var i = 0; i < N; i++) data[i] = (i * 131 + (i >> 4)) & 255;
var crc = -1;
for (var i = 0; i < N; i++) crc = tab[(crc ^ data[i]) & 255] ^ (crc >>> 8);
mix(~crc);
var a = 1, b = 0;
for (var i = 0; i < N; i++) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
mix(b * 65536 + a);
print('crc-checksum ' + h);
