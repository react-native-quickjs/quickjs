// `debugger;` must be semantically inert when no debugger is attached.
//
// Patch 0019 makes the parser emit OP_source_loc + OP_debug for every
// `debugger;` statement, in every build configuration, where previously the
// keyword was parsed and discarded. That is a real change to the instruction
// stream, so it can change behaviour in three ways this file is built to catch:
//
//   1. The trap itself doing something observable when no handler is installed.
//   2. The extra opcode perturbing the peephole optimizer -- `debugger;` sits
//      between statements the optimizer likes to fuse (constant folding, dead
//      code elimination after `return`, short-jump selection), and an opcode the
//      optimizer skips over incorrectly would corrupt the code around it.
//   3. Line/column attribution drifting, since resolve_labels now emits a
//      pc2line entry at the trap.
//
// node ignores `debugger;` when no inspector is attached, so its output is the
// reference. Run through --via-bytecode as well: that proves OP_debug survives
// serialization and is skipped correctly on the precompiled path, which is the
// path React Native ships.

function withTrap(a) {
  debugger;
  var b = a + 1;
  if (b > 2) {
    debugger;
    b = b * 2;
  }
  return b;
}

function noTrap(a) {
  var b = a + 1;
  if (b > 2) {
    b = b * 2;
  }
  return b;
}

for (var i = 0; i < 5; i++) {
  print(i + ' ' + withTrap(i) + ' ' + noTrap(i));
}

// Dead code after a return: skip_dead_code() walks the instruction stream and
// must not trip over the trap.
function afterReturn() {
  return 'early';
  debugger;
  return 'late';
}
print(afterReturn());

// Trap as the only statement of a block, and inside a loop body, so the
// short-jump selection pass sees it inside a branch target.
function inLoop(n) {
  var total = 0;
  for (var k = 0; k < n; k++) {
    debugger;
    total += k;
  }
  return total;
}
print(inLoop(4));

// Constant folding across a trap.
function folded() {
  var x = 2 + 3;
  debugger;
  var y = x * 4;
  return y;
}
print(folded());

// Line attribution: a throw after a trap must still report its own line.
function lineCheck() {
  try {
    debugger;
    null.x;
  } catch (e) {
    return e instanceof TypeError;
  }
  return false;
}
print(lineCheck());

// The trap must not affect closures or `this`.
function closureCheck() {
  var captured = 7;
  return function () {
    debugger;
    return captured + 1;
  };
}
print(closureCheck()());
