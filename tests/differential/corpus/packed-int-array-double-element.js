// Fails without patch 0473 only in a QJS_PACKED_ARRAYS build with -fsanitize=float-cast-overflow (the printed output is the same either way).
// Storing a double outside the int32 range into a packed int32 array: js_arr_packed_num_int must test the range before it converts (the conversion is undefined behaviour out of range, and an optimizing compiler may take the round-trip test as always true).
var vals = [1e10, -2147483649, 2147483648, -2147483648, 2147483647, NaN, -0, 0, 1.5, Infinity, -Infinity, 4294967297, 1e300];
var out = [];
for (var k = 0; k < vals.length; k++) {
  var v = vals[k];
  var a = [1, 2, 3];
  a[1] = v;
  var b = [1, 2, 3];
  b.push(v);
  var c = [1, 2, 3];
  c.fill(v);
  var d = [1, 2, 3, v];
  var e = Array.of(1, 2, v);
  var f = [1, 2, 3];
  f.unshift(v);
  out.push([a, b, c, d, e, f].map(function (x) { return x.map(function (y) { return Object.is(y, -0) ? '-0' : String(y); }).join(':'); }).join('|'));
}
print(out.join('\n'));
