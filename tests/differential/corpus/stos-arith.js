// Differential corpus for the static top-of-stack cache: arithmetic whose
// operands are locals, arguments and immediate integers.
//
// WHY THIS FILE.  The cache is only observable when a value is produced and
// consumed inside one basic block.  If a producer's result is read from the
// wrong place the answer is wrong but nothing crashes, so the printed values
// are the whole test.  Every shape here is one the transform is willing to
// rewrite: load, load, binary, and a store that consumes the result.

function show(k, v) { print(k + '=' + v); }

function locals(a, b) {
  var x = a, y = b;
  var t = x + y;
  var u = t * 2;
  return u - x;
}
show('locals', locals(3, 4));

function argsOnly(a, b, c) {
  return (a * b) + (c - a);
}
show('argsOnly', argsOnly(2, 3, 5));

function mixed(a, b) {
  return (a + 1) * (b - 2) - 3;
}
show('mixed', mixed(4, 6));

function deep(a) {
  var r = a + 1;
  r = r * r;
  r = r - a;
  r = r + 7;
  r = r * 3;
  return r;
}
show('deep', deep(5));

function overflow(a, b) {
  return (a * b) + (a - b);
}
show('overflow', overflow(0x40000000, 0x40000000));

function floatMix(a, b) {
  return (a + 0.5) * (b - 3);
}
show('floatMix', floatMix(2, 9));

function stores(a) {
  var x = a + 1;
  var y = x + 2;
  var z = y + x;
  return z - y + x;
}
show('stores', stores(10));

function inLoop(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t = t + i;
    t = t * 2;
    t = t - 1;
  }
  return t;
}
show('inLoop', inLoop(5));

function bigintMul(a, b) { return a * b; }
show('bigintMul', bigintMul(3n, 4n));

function bigintSub(a, b) { return a - b; }
show('bigintSub', bigintSub(10n, 25n));

function bigintOverflow(a) { return a * a; }
show('bigintOverflow', bigintOverflow(0x40000000n));
