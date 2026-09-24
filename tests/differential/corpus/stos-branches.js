// Differential corpus for the static top-of-stack cache across control flow.
//
// WHY THIS FILE.  The cache state at a block entry has to be the same on every
// edge into that block, or one predecessor reads a value the other never
// produced.  These shapes put a merge, a back edge and a short-circuit in the
// way of the arithmetic: if the compiler lets a cached value cross a branch,
// one of the printed answers changes.

function show(k, v) { print(k + '=' + v); }

function merge(x) {
  var r;
  if (x) {
    r = (1 + 2) + 3;
  } else {
    r = 4;
  }
  return r + 5;
}
show('merge1', merge(1));
show('merge2', merge(0));

function backEdge(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    t = t + 1;
    if (i & 1) t = t * 3;
    else t = t - 2;
  }
  return t;
}
show('backEdge', backEdge(7));

function shortCircuit(a, b) {
  return (a && b) + 1;
}
show('sc1', shortCircuit(1, 2));
show('sc2', shortCircuit(0, 2));
show('sc3', shortCircuit(1, 0));

function ternaryPick(a) {
  return (a > 2 ? a * 2 : a + 10) + 1;
}
show('tern1', ternaryPick(5));
show('tern2', ternaryPick(1));

function nestedLoops(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    for (var j = 0; j < n; j++) {
      t = t + (i * j);
    }
  }
  return t;
}
show('nested', nestedLoops(4));

function loopWithBreak(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    if (i === 3) break;
    t = t + i * 2;
  }
  return t;
}
show('break', loopWithBreak(10));

function loopWithContinue(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    if (i & 1) continue;
    t = t + i;
  }
  return t;
}
show('continue', loopWithContinue(8));

function whileLoop(n) {
  var t = 0;
  while (n > 0) {
    t = t + n * 2;
    n = n - 1;
  }
  return t;
}
show('while', whileLoop(5));

function doWhile(n) {
  var t = 1;
  do {
    t = t * 2 + 1;
    n = n - 1;
  } while (n > 0);
  return t;
}
show('doWhile', doWhile(4));

function switchMerge(x) {
  var r = 0;
  switch (x) {
    case 0: r = 1 + 2; break;
    case 1: r = 3 * 4; break;
    default: r = 5 - 6;
  }
  return r + 100;
}
show('switch0', switchMerge(0));
show('switch1', switchMerge(1));
show('switch2', switchMerge(9));
