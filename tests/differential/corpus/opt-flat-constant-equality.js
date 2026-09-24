// Differential corpus for pass 5's folding of equality between the constants
// the pass models: int32, Boolean, null and undefined.
//
// WHY THIS FILE.  `==` is the one place JavaScript compares across types, and
// its table is not the one intuition suggests: `null` and `undefined` are
// equal to each other and to nothing else, a Boolean is its number, and a
// number is never a null. A fold that got any of those wrong would change a
// branch, so every row of the table is here, in both directions and with both
// `==` and `!=`, and the file is run against node as well as qjsc -O.

function show(k, v) { print(k + '=' + v); }

var rows = [
  [undefined, undefined], [null, null], [undefined, null], [null, undefined],
  [undefined, 0], [0, undefined], [null, 0], [0, null],
  [undefined, false], [false, undefined], [null, false], [false, null],
  [true, 1], [1, true], [false, 0], [0, false], [true, 0], [false, 1],
  [true, true], [false, false], [true, false],
  [1, 1], [1, 2], [0, -0],
];
for (var i = 0; i < rows.length; i++) {
  var a = rows[i][0], b = rows[i][1];
  show('eq' + i, (a == b) + ',' + (a != b) + ',' + (a === b) + ',' + (a !== b));
}

/* the same comparisons in a loop, where the folding has to keep the branch
   and the value in step */
function counted(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    if (undefined == null) t += 1;
    if (null == 0) t += 100;
    if (true == 1) t += 1000;
    if (false == 1) t += 10000;
    if (undefined != null) t += 100000;
    if (1 == 1) t += 1000000;
  }
  return t;
}
show('counted', counted(3));
