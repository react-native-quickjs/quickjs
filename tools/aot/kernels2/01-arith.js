// Arithmetic: int fast paths, overflow to double, unary ops, bit ops.
function k() {
  var a = 7, b = 3, s = 0, i;
  for (i = 0; i < 1000; i++) {
    s = (s + a * b) | 0;
    s = (s - (i & 15)) | 0;
    s = (s ^ (i << 1)) | 0;
    s = (s >>> 3) | 0;
  }
  var d = 0.5;
  for (i = 0; i < 100; i++) d = d * 1.25 + 0.25;
  var n = 0x7ffffff0;
  n = (n + 0x20) | 0;              // wraps to a negative int
  var big = 2147483647 + 1;        // not an int: a double
  var neg = -0;
  var inf = 1 / 0;
  return [s, d, n, big, 1 / neg === -Infinity, inf === Infinity, 5 % 3, -7 % 3];
}
print(k().join(","));
