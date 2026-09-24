// Differential corpus for pass 5's folding of a comparison the intervals
// already decide: a guard inside a loop whose counter is bounded by the test
// around it, and the same against a value the pass has bounded another way.
//
// WHY THIS FILE.  `for (i = 0; i < 8; i++) { if (i < 8) ... }` has a branch
// that is always taken and `if (i > 20)` one that never is. Deciding either
// from the interval alone is exact only because both sides are int32 and the
// interval is a proof, not an estimate -- so every row here prints the value
// the branch produced, and the loop bounds include the boundaries where the
// proof stops: a parameter bound the pass cannot name, a bound with the guard
// sitting exactly on it, and a count that walks off the end.

function show(k, v) { print(k + '=' + v); }

function guarded(n) {
  var t = 0;
  for (var i = 0; i < 8; i++) {
    if (i > 20) t += 1000;
    if (i >= 0) t += 1;
    if (i < 8) t += 2;
    if (i <= 7) t += 4;
    if (i >= 8) t += 8000;
    t += i;
  }
  return t + n;
}
show('constant bound', guarded(0));

function parameterBound(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    if (i > 1000000) t += 1000;
    if (i < 0) t += 10000;
    t += i;
  }
  return t;
}
show('parameter bound', parameterBound(4) + ',' + parameterBound(0));

function boundaryExact(n) {
  var t = 0;
  for (var i = 0; i <= n; i++) {
    if (i < 0) t += 1;          /* never */
    if (i <= n) t += 2;         /* always, and the guard sits on the bound */
    if (i >= n) t += 4;         /* at the end only */
    t += i;
  }
  return t;
}
show('on the bound', boundaryExact(3));

function nestedGuards(m) {
  var t = 0;
  for (var i = 0; i < 6; i++) {
    for (var j = 0; j < 4; j++) {
      if (i > 100) t += 1000;
      if (j < 4) t += 1;
      t += i * 4 + j;
    }
  }
  return t + m;
}
show('nested', nestedGuards(9));

function walkOff(n) {
  var t = 0;
  var i = 0;
  while (i < 5) {
    if (i >= 5) t += 1000;
    t += 1;
    i = i + 1;
  }
  return t + n;
}
show('walk off the end', walkOff(0));
