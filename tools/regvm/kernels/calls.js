function (iterations) {
  function arity0() { return 1; }
  function arity1(a) { return a + 1; }
  function arity4(a, b, c, d) { return a + b + c + d; }
  function arity8(a, b, c, d, e, f, g, h) { return a + b + c + d + e + f + g + h; }

  var acc = 0;
  var i = 0;
  while (i < iterations) {
    acc = acc + arity0();
    acc = acc + arity1(i);
    acc = acc + arity4(1, 2, 3, 4);
    acc = acc + arity8(1, 2, 3, 4, 5, 6, 7, 8);
    i = i + 1;
  }
  return acc;
}
