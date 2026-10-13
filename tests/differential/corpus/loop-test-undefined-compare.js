/*
 * Differential corpus: a loop whose header test is a strict compare against
 * `undefined`.
 *
 * The register backend fuses `x !== undefined` followed by a branch into a
 * single tag test, and separately rotates a loop so its latch repeats the
 * header test with the opposite sense. The tag test has no inverted form, so
 * a rotation that flipped its sense the way it flips an ordinary comparison
 * produced a sub-operation no handler knew, and the first trip round the loop
 * threw `InternalError: register VM: unhandled binary op`. Every loop here has
 * that shape, in both senses and with the operands in both orders.
 */
function walk(o) { var n = 0; while (o !== undefined) { n++; o = o.next; } return n; }
function walkRev(o) { var n = 0; while (undefined !== o) { n++; o = o.next; } return n; }
function walkVoid(o) { var n = 0; while (o !== void 0) { n++; o = o.next; } return n; }
function fill(o) { var n = 0; while (o === undefined) { n++; o = 1; } return n; }
function fillRev(o) { var n = 0; while (undefined === o) { n++; if (n === 3) o = null; } return n; }
function walkFor(o) { var n = 0; for (; o !== undefined; o = o.next) n++; return n; }
function walkDo(o) { var n = 0; do { n++; o = o.next; } while (o !== undefined); return n; }
function walkBreak(o) {
  var n = 0;
  while (o !== undefined) { if (o.stop) break; n++; o = o.next; }
  return n;
}
function walkNested(o) {
  var n = 0;
  while (o !== undefined) {
    var p = o.inner;
    while (p !== undefined) { n += 10; p = p.next; }
    n++; o = o.next;
  }
  return n;
}

var list = { next: { next: { next: undefined } } };
var stop = { next: { next: { stop: true, next: undefined } } };
var nested = { inner: { next: {} }, next: { inner: undefined, next: { inner: {} } } };

var out = [];
for (var rep = 0; rep < 3; rep++) {
  out.push(walk(list), walk(undefined), walkRev(list), walkVoid(list),
           fill(undefined), fill(0), fillRev(undefined), walkFor(list),
           walkDo(list), walkBreak(stop), walkNested(nested));
}
print(out.join(","));
