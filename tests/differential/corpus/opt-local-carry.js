// Differential corpus for bytecode optimizer pass 4: carrying a stored value
// on the stack, the int32 zero test, and the precomputed-Boolean conjunction
// (patch 0115, `js_opt_carry_round()`).
//
// WHY THIS FILE.  Pass 4 deletes a store and the load that reads it back,
// leaving the value on the stack between them.  That is only correct if the
// value really is still there when the read would have run, if nothing takes
// another exit in between, and if the consumer cannot tell that the value it
// gets was already on the stack rather than reloaded.  Each case below is one
// of the ways that goes wrong, written so that a rewrite which missed it
// prints a different line rather than crashing.
//
//   A  the shapes the pass is for: an adjacent pair, a pair with a
//      stack-neutral branch between them, a pair with the accumulator loaded
//      above it (the consumer is `add`, which is commutative on Numbers), a
//      test against int32 zero, and a conjunction of two computed Booleans.
//   N  the shapes it must refuse: two reads, a read before the store, an exit
//      between them, a consumer that is not commutative, a test against zero
//      on a value that may be a float, a conjunction of values that are not
//      Booleans, and a property read in between.

function show(k, v) { print(k + '=' + v); }

/* ---- A: what the pass changes ---------------------------------------- */

function adjacent(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var live = i & 1;
    t += live;
  }
  return t;
}
show('A1 adjacent store and load', adjacent(9));

function acrossBranch(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var a = i + 1;
    if (i & 1) t += 0;
    t += a;
  }
  return t;
}
show('A2 across a stack-neutral branch', acrossBranch(9));

function aboveTheValue(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var v = i & 3;
    t = t + v;
  }
  return t;
}
show('A3 accumulator loaded above it', aboveTheValue(9));

function zeroTest(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    if ((i & 1) === 0) t += 1;
    else t += 2;
  }
  return t;
}
show('A4 int32 test against zero', zeroTest(9));

function conjunction(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var a = (i & 1) === 0;
    var b = (i & 2) === 0;
    if (a && b) t += 1;
    else t += 2;
  }
  return t;
}
show('A5 precomputed Booleans joined', conjunction(9));

/* ---- N: what the pass must refuse ------------------------------------ */

function twoReads(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var a = i * 3;
    t += a + a;
  }
  return t;
}
show('N1 two reads of the store', twoReads(9));

function readBeforeStore(n) {
  var a = 0;
  var t = 0;
  for (var i = 0; i < n; i++) {
    t += a;
    a = i;
  }
  return t + a;
}
show('N2 read before the store', readBeforeStore(9));

function exitBetween(n) {
  var a = n * 2;
  if (n > 4) return a + 100;
  return a;
}
show('N3 an exit between them', exitBetween(9) + ',' + exitBetween(2));

function notCommutative(n) {
  var t = 0;
  for (var i = 1; i <= n; i++) {
    var d = i;
    t = d - t;
  }
  return t;
}
show('N4 subtraction above the value', notCommutative(5));

function floatZero(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var x = i * 0.5;
    if (x === 0) t += 1;
    else t += 2;
  }
  return t;
}
show('N5 test on a value that may be a float', floatZero(9));

function numbersJoined(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var a = i & 3;
    var b = (i + 1) & 3;
    if (a && b) t += 1;
    else t += 2;
  }
  return t;
}
show('N6 values that are not Booleans', numbersJoined(9));

var box = { k: 1 };
function propertyBetween(n) {
  var a = n * 3;
  var b = box.k;
  return a + b;
}
show('N7 a property read in between', propertyBetween(9));

function keptAlive(n) {
  var a = { v: n };
  var t = a.v + 1;
  a.v = t;
  return a.v;
}
show('N8 a value the object still holds', keptAlive(9));

/* A store outside a loop whose value is read inside it: the read takes the
   same stack entry on every pass, and only the first pass has it. Every test
   the carry makes has to refuse this, or the loop adds nothing after the
   first iteration. */
function storeOutsideLoop(n) {
  var a = n * 3;
  var t = 0;
  var i = 0;
  do {
    t += a;
    i++;
  } while (i < 3);
  return t + ',' + a;
}
show('N9 store outside the do/while', storeOutsideLoop(2));

function storeBeforeWhile(n) {
  var a = n + 7;
  var t = 0;
  var i = 0;
  while (i < 3) {
    t = t + a;
    i++;
  }
  return t + ',' + a;
}
show('N10 store before a while', storeBeforeWhile(2));

function subtractedUpdate(n) {
  var t = 100;
  for (var i = 1; i <= n; i++) {
    var v = i * 2;
    t = t - v;
  }
  return t;
}
show('N11 an update the fused form cannot name', subtractedUpdate(5));

function valueUsedAfter(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var v = (i & 3) + 1;
    t += v;
    t += v;
  }
  return t;
}
show('N12 the value read twice', valueUsedAfter(9));

/* A path that leaves the read behind. The value is carried down one arm of a
   branch and the other arm continues the loop, so the next iteration would
   find the old value still on the stack -- the stack the engine sizes then
   has two depths for one place. Every path from the store has to reach the
   read, and these are the paths that do not: a `continue`, a `break`, and a
   return. */
function continuePastTheRead(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var v = i | 0;
    if (i & 1) t += v;
    else continue;
  }
  return t;
}
show('N13 a continue that skips the read', continuePastTheRead(9));

function breakPastTheRead(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var v = (i | 0) + 1;
    if (i & 1) t += v;
    else break;
  }
  return t;
}
show('N14 a break that skips the read', breakPastTheRead(9));

function returnPastTheRead(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var v = (i & 0) | n;
    if (i & 2) t += v;
    else if (i > 3) return t * 10;
  }
  return t;
}
show('N15 a return that skips the read', returnPastTheRead(9) + ',' + returnPastTheRead(2));
