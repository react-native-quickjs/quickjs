/*
 * An unsigned shift produces a value above 2147483647, and no integer proof
 * about it may be taken for a signed int32.
 *
 * `x >>> 0` is ToUint32: for a negative x it is a large positive number, not
 * x itself. An optimizer that tags it as an int32 will decide that a
 * following `| 0` or `>> 0` changes nothing and drop it, which changes the
 * answer -- 30 becomes 21474836510 here.
 */
var shifts = [-5, -1, -2147483648, 0, 1, 2147483647, 3.7, -0.5, NaN];
var out = [];

for (var k = 0; k < shifts.length; k++) {
  var v = shifts[k];
  var u = v >>> 0;
  out.push(u, u | 0, u >> 0, u >>> 0, (v >>> 1), (v >>> 33), (v << 1), (v & 255));
}

function sumShr(n) {
  var t = 0;
  for (var i = -5; i < n; i++) {
    var y = i >>> 0;
    t = t + (y | 0) + (y >> 0) + y;
  }
  return t;
}

print(out.join(","));
print(sumShr(10), sumShr(0), sumShr(1));
print(((-1 >>> 0) | 0), (4294967295 | 0), (2147483648 >> 0), (2147483647 >>> 0));
