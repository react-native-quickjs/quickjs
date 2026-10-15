// Arrays: literals, elements, length, push/pop, holes, typed arrays.
function k(n) {
  var a = [1, 2, 3, 4], s = 0, i;
  for (i = 0; i < n; i++) {
    s = (s + a[i & 3]) | 0;
    a[i & 3] = (a[i & 3] + 1) | 0;
  }
  var b = [];
  for (i = 0; i < 10; i++) b.push(i * i);
  var popped = b.pop();
  var ta = new Int32Array(8);
  for (i = 0; i < 8; i++) ta[i] = i * 3;
  var sum = 0;
  for (i = 0; i < ta.length; i++) sum = (sum + ta[i]) | 0;
  var holes = [1, , 3];
  return [s, a.join(","), b.length, popped, sum, ta.length, 1 in holes, holes.length];
}
print(k(20000));
