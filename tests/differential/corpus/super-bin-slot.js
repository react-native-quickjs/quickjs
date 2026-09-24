/* Phase-2 superinstruction coverage.  The supported complete shapes are
   load/load-or-immediate, binary operation, and direct slot store. */

function show(name, value) { print(name + '=' + value); }

function slotSlot(a, b) {
  var x = a, y = b, z = x * y;
  z = x + y;
  return z;
}
function slotImm(a) {
  var x = a, y = x * 14;
  y = x + -7;
  return y;
}
function stringAdd(a) {
  var x = a;
  x = x + 1;
  return x;
}
function aliases(a, b) {
  var x = a, y = b;
  x = x * y;
  y = x + y;
  return y;
}
function accum(a, b, c) {
  var x = a;
  x = x + (b * c);
  return x;
}
function same(a) {
  var x = a;
  x = x * x;
  return x;
}
function inLoop(n) {
  var x = 1, y = 2, i;
  for (i = 0; i < n; i++) {
    x = x * y;
    x = x + 1;
  }
  return x;
}
function beforeBranch(a, b) {
  var x = a * b;
  x = x + 1;
  if (x > 10)
    return x;
  return x + 2;
}
function beforeCall(a, b) {
  var x = a * b;
  x = x + 3;
  return String(x);
}
function beforeTry(a, b) {
  var x = a * b;
  try {
    x = x + 4;
  } catch (e) {
    x = x + 5;
  }
  return x;
}

show('slotSlot', slotSlot(3, 4));
show('slotImm', slotImm(5));
show('aliases', aliases(3, 4));
show('accum', accum(1, 2, 3));
show('same', same(6));
show('inLoop', inLoop(4));
show('beforeBranch', beforeBranch(3, 4));
show('beforeCall', beforeCall(3, 4));
show('beforeTry', beforeTry(3, 4));
show('float', slotSlot(1.5, 2));
show('stringAdd', stringAdd('x'));
show('bigint', slotSlot(3n, 4n));
try { slotSlot(3n, 4); } catch (e) { show('mixedBigInt', e.name); }
try { slotSlot(Symbol('x'), 2); } catch (e) { show('symbol', e.name); }

/* This is deliberately not a complete supported shape: its call must remain
   visible and must not be swallowed by the matcher. */
function unsupported(a, b) {
  var x = a * b;
  x = Number(x) + 1;
  return x;
}
show('unsupported', unsupported(2, 3));
