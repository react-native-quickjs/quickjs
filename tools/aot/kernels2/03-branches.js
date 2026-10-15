// Branches: compares, null and nullish tests, short-circuit, conditionals.
function k(n) {
  var s = 0, x = null, y = 1, i, o = { v: 5 };
  for (i = 0; i < n; i++) {
    if (x === null) s = (s + 1) | 0;
    if (x !== null) s = (s - 1) | 0;
    if (y == null) s = (s + 100) | 0;
    s = (s + (x ?? 7)) | 0;
    s = (s + (y || 3)) | 0;
    s = (s + (i > n / 2 ? 1 : 2)) | 0;
    if (i < 3) s = (s + 5) | 0; else if (i < 6) s = (s + 6) | 0; else s = (s + 7) | 0;
    s = (s + (o.v > 3 ? 1 : 0)) | 0;
  }
  return [s, x === undefined, y < 2, "a" < "b", 1 === 1.0];
}
print(k(1000));
