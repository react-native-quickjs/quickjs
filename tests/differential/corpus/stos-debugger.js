// Differential corpus for the static top-of-stack cache and the debugger trap.
//
// WHY THIS FILE.  `debugger;` compiles to a trap opcode that sits in the middle
// of a function, and the static-TOS transform emits extra instructions around
// it.  A trap is a point where the engine can re-enter the embedder, so a
// cached value has to be in memory before it, and the peephole pass must not
// fold across it.  The output is compared against node, which ignores the trap
// when no inspector is attached.

function show(k, v) { print(k + '=' + v); }

function trapBetweenArith(a) {
  var t = a + 1;
  debugger;
  t = t * 2;
  debugger;
  t = t - 3;
  return t;
}
show('trapBetweenArith', trapBetweenArith(5));

function trapInLoop(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t = t + i;
    debugger;
    t = t * 2;
  }
  return t;
}
show('trapInLoop', trapInLoop(4));

function trapBeforeThrow(a) {
  var t = a * 2;
  debugger;
  try {
    null.x;
  } catch (e) {
    return t + ':' + (e instanceof TypeError);
  }
  return 'none';
}
show('trapBeforeThrow', trapBeforeThrow(11));

function trapInBranch(a) {
  var t;
  if (a) {
    debugger;
    t = 1 + 2;
  } else {
    t = 3 + 4;
  }
  return t + 100;
}
show('trapInBranch1', trapInBranch(1));
show('trapInBranch2', trapInBranch(0));

function trapAfterReturn() {
  var t = 4 + 5;
  return t;
  debugger;
  return 0;
}
show('trapAfterReturn', trapAfterReturn());
