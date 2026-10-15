// Loops: counted loops, nested loops, induction variables, postfix increments.
function k(n) {
  var s = 0, i, j, acc = 0;
  for (i = 0; i < n; i++) {
    s = (s + i) | 0;
    for (j = 0; j < 4; j++) acc = (acc + i * j) | 0;
  }
  var k2 = 0, t = 0;
  while (k2 < n) { t = (t + k2) | 0; k2 += 3; }
  var d = 0;
  do { d++; } while (d < 10);
  var idx = 0, arr = [0, 0, 0, 0];
  for (i = 0; i < 8; i++) arr[idx++] = i;
  return [s, acc, t, d, arr.join(",")];
}
print(k(100000));
