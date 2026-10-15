// Mixed: template literals, Math, BigInt, spread, destructuring, JSON.
function k(n) {
  var s = 0, i, arr = [1, 2, 3, 4, 5];
  for (i = 0; i < n; i++) {
    s = (s + Math.imul(i & 7, 3)) | 0;
    s = (s + Math.max(i & 3, 1)) | 0;
  }
  var [a, b, ...rest] = arr;
  var { x, y = 2 } = { x: 1 };
  var big = 2n ** 40n;
  var sum = 0;
  for (const v of rest) sum = (sum + v) | 0;
  return [s, `t${a}-${b}-${y}`, rest.length, sum, big.toString(),
          JSON.stringify({ x, y }), Math.sqrt(2) > 1.41];
}
print(k(4000));
