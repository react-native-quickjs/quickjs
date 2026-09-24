// Differential corpus for bytecode optimizer pass 7: one virtual object built
// from several literals on different paths (patch 0133, `js_opt_vobj_round()`
// with a multi-site candidate).
//
// WHY THIS FILE.  Pass 7 takes apart an object the program builds on the arms
// of a branch and then names with a single local -- `var p = c ? { x: 1 } :
// { x: 2, y: 3 }`.  The two literals become writes of one set of frame slots,
// and the arms meet at the store with the object gone.  That is only correct
// if the slots really are the object on every path: a field one arm writes and
// another does not must read `undefined` on the path that skips it, *every*
// time the arm runs, even after another execution filled the slot; a field
// only one arm's value is read as is a value; and the object itself must still
// be unobservable, or it is not an object that can be taken apart at all.
//
// Each case below prints a line that moves if the rewrite got one of those
// wrong.  The A cases are the shapes the pass is for (a correct optimizer
// changes them and the output must not move); the N cases are the uses that
// keep the object real.  A12 and A13 also cover the two later pieces: a chain
// of nesting, which costs one round per layer, and a literal the inliner
// carried in, which has to be given a site of the caller's before the object
// round can see it at all.

function show(k, v) { print(k + '=' + v); }

/* ---- A: the shapes the pass is for ------------------------------------- */

function sameShape(c) {
  var p = c ? { x: 1, y: 2 } : { x: 3, y: 4 };
  return p.x * 100 + p.y;
}
show('A1 ternary, same shape', sameShape(1) + ',' + sameShape(0));

function ifElse(c) {
  var p;
  if (c) { p = { x: 1, y: 2 }; } else { p = { x: 3, y: 4 }; }
  return p.x * 100 + p.y;
}
show('A2 if, same shape', ifElse(1) + ',' + ifElse(0));

/* One arm writes a field the other does not.  The skipped field reads
   `undefined` -- and must, on every path, not only the first time. */
function missingField(c) {
  var p = c ? { x: 1 } : { x: 3, y: 4 };
  return p.x * 100 + (p.y === undefined ? -1 : p.y);
}
show('A3 ternary, field one arm lacks',
     missingField(1) + ',' + missingField(0));

/* The same shape in a loop.  A slot the first iteration wrote must read
   `undefined` on the next iteration's other arm, which is what a per-site
   initialisation of the missing fields is for. */
function missingInLoop(n) {
  var out = '';
  for (var i = 0; i < n; i++) {
    var p = (i & 1) ? { x: i } : { x: i, y: 100 + i };
    out += p.x + (p.y === undefined ? 'u' : p.y) + ',';
  }
  return out;
}
show('A4 missing field across iterations', missingInLoop(6));

function ifElseMixed(c) {
  var p;
  if (c) { p = { a: 1 }; } else { p = { a: 2, b: 3 }; }
  return p.a * 10 + (p.b === undefined ? 0 : p.b);
}
show('A5 if, field one arm lacks', ifElseMixed(1) + ',' + ifElseMixed(0));

function threeArms(c) {
  var p;
  if (c === 0) { p = { x: 1, y: 2 }; }
  else if (c === 1) { p = { x: 3, y: 4 }; }
  else { p = { x: 5, y: 6 }; }
  return p.x * 100 + p.y;
}
show('A6 three arms', threeArms(0) + ',' + threeArms(1) + ',' + threeArms(9));

function aliasedAfter(c) {
  var p = c ? { x: 1, y: 2 } : { x: 3 };
  var q = p;
  return q.x * 100 + (q.y === undefined ? -1 : q.y);
}
show('A7 alias after the merge', aliasedAfter(1) + ',' + aliasedAfter(0));

function everyType(c) {
  var p = c ? { n: null, u: undefined, s: 'text' }
            : { n: 5, u: true, s: 'longer' };
  return p.n + ',' + p.u + ',' + p.s.length;
}
show('A8 every field type', everyType(1) + ',' + everyType(0));

function mkObj(n) { return { n: n }; }
function referenceField(c, n) {
  var p = c ? { held: mkObj(n) } : { held: mkObj(n + 1), extra: mkObj(n + 2) };
  return p.held.n + (p.extra === undefined ? 0 : p.extra.n);
}
show('A9 reference-valued field', referenceField(1, 7) + ',' + referenceField(0, 7));

function reordered(c) {
  /* The union is the first site's order, and the second site's order is a
     subsequence of it, so both fold into one layout. */
  var p = c ? { a: 1, b: 2 } : { a: 5, b: 6 };
  return p.a * 10 + p.b;
}
show('A10 same fields, the other spelling', reordered(1) + ',' + reordered(0));

function nestedInLoop(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var s = (i & 1) ? { padding: 0 } : { padding: 8, margin: 4 };
    t += s.padding + (s.margin === undefined ? 0 : s.margin);
  }
  return t;
}
show('A11 a differing shape in a loop', nestedInLoop(8));

/* ---- N: the object stays real ------------------------------------------- */

var sink = null;
function mergedEscapes(c) {
  var p = c ? { x: 1 } : { x: 2 };
  sink = p;
  return p.x;
}
show('N1 the merged local escapes', mergedEscapes(1) + ',' + mergedEscapes(0) + ',' + sink.x);

function returned(c) {
  var p = c ? { x: 1, y: 2 } : { x: 3, y: 4 };
  return p;
}
var r = returned(1);
show('N2 returned', r.x + ',' + r.y);

var other = { x: 9 };
function compared(c) {
  var p = c ? { x: 1 } : { x: 2 };
  return (p === other) + ',' + p.x;
}
show('N3 identity compared', compared(1) + ',' + compared(0));

function enumerated(c) {
  var p = c ? { x: 1 } : { x: 2 };
  var k = '';
  for (var key in p) k += key;
  return k + ',' + p.x;
}
show('N4 for in', enumerated(1) + ',' + enumerated(0));

/* A property order no single layout can hold: the arms spell the fields in
   opposite orders, so the candidate is refused and the objects stay.  The
   output is the same either way. */
function oppositeOrder(c) {
  var p = c ? { a: 1, b: 2 } : { b: 5, a: 6 };
  return p.a * 10 + p.b;
}
show('N5 properties in opposite orders', oppositeOrder(1) + ',' + oppositeOrder(0));

/* The aggregate is stored into a real object (the real object escapes), so it
   cannot be taken apart: a field of a real object cannot hold slots. */
function heldInRealObject(c) {
  var s = c ? { p: 1 } : { p: 2, q: 3 };
  var o = { s: s };
  return o.s.p + (o.s.q === undefined ? 0 : o.s.q) + Object.keys(o).length;
}
show('N6 stored in a real object', heldInRealObject(1) + ',' + heldInRealObject(0));

function passedToUnknown(c) {
  var p = c ? { x: 1 } : { x: 2 };
  return p.x + Math.min(p.x, 10);
}
show('N7 field read, then a call on the value', passedToUnknown(1) + ',' + passedToUnknown(0));

/* A branch-merged literal held in a field of a nested literal, read through
   both.  Each layer has to become a slot before the one inside it can go, so
   this only folds if the round is offered enough rounds to take the chain one
   layer at a time. */
function nestedChain(c, v) {
  var style = c ? { padding: 1 } : { padding: 2, margin: 3 };
  var node = { type: 7, props: { style: style, value: v } };
  return node.props.style.padding +
         (node.props.style.margin === undefined ? 0 : node.props.style.margin) +
         node.props.value;
}
show('A12 nested branch-merged literal', nestedChain(1, 10) + ',' + nestedChain(0, 10));

/* A literal the inliner carried in.  Its own function built it on the fast
   path, and the splice has to record a site for it or the copy builds it the
   slow way -- and, once it has, the object round has to be able to take it
   apart where the call was. */
function inlinedHolder(n) {
  function make() { return { x: n, y: n + 1 }; }
  var t = 0;
  for (var i = 0; i < 4; i++) {
    var o = make();
    t += o.x + o.y;
  }
  return t;
}
show('A13 literal carried by inlining', inlinedHolder(5));

/* The field itself escapes, so neither it nor the object holding it can be
   taken apart. */
var nestedSink = null;
function nestedFieldEscapes(c) {
  var style = c ? { padding: 1 } : { padding: 2 };
  var node = { props: { style: style } };
  nestedSink = node.props.style;
  return node.props.style.padding;
}
show('N8 a nested field escapes', nestedFieldEscapes(1) + ',' + nestedFieldEscapes(0) + ',' + nestedSink.padding);

