// Exceptions: throw and catch, finally, exceptions out of loops and calls.
function inner(n) {
  if (n < 0) throw new Error("neg");
  return n * 2;
}
function k(n) {
  var s = 0, i, fin = 0;
  for (i = 0; i < n; i++) {
    try {
      s = (s + inner(i & 7)) | 0;
      if ((i & 3) === 0) throw "odd";
    } catch (e) {
      s = (s - 1) | 0;
    } finally {
      fin = (fin + 1) | 0;
    }
  }
  try { inner(-1); } catch (e) { s = (s + (e.message === "neg" ? 1 : 0)) | 0; }
  return [s, fin];
}
print(k(5000));
