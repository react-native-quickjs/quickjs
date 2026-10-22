var h = 0;
function mix(x) { h = (h * 31 + (x | 0) + 7) | 0; }
function mixd(x) { mix(x !== x || x === Infinity || x === -Infinity ? 0 : Math.floor(x * 256) % 1000003); }
function mixs(s) { s = String(s); for (var q = 0; q < s.length; q++) mix(s.charCodeAt(q)); }
var N = 119824;
// 8 queens via bitmasks
function queens(n, row, cols, d1, d2) {
  if (row == n) return 1;
  var avail = ~(cols | d1 | d2) & ((1 << n) - 1), c = 0;
  while (avail) { var bit = avail & -avail; avail ^= bit; c += queens(n, row + 1, cols | bit, ((d1 | bit) << 1) & 0xffff, (d2 | bit) >> 1); }
  return c;
}
mix(queens(8, 0, 0, 0, 0));
var t = 0;
for (var i = 0; i < N; i++) {
  var b = (i * 2654435761) | 0, pc = 0;
  while (b) { b &= b - 1; pc++; }
  t += pc;
  var lo = (i * 40503) & 0xffff, hi = (i * 9973) & 0xffff;
  t += ((lo << 16 | hi) >>> 0) % 1009;
  t += Math.clz32(i + 1) + (31 - Math.clz32(i + 1));
}
mix(t);
print('bitboard ' + h);
