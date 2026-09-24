// Differential corpus for bytecode optimizer pass 3: object escape analysis,
// field forwarding and scalar replacement (patch 0114, `js_opt_vobj_round()`).
//
// WHY THIS FILE.  Pass 3 deletes an allocation.  `var p = { x: 1 }` whose
// object nothing can observe becomes one slot holding 1, and every read and
// write of a field becomes a read and write of that slot.  That is only
// correct if the object really is unobservable, and "unobservable" is a claim
// about every use the program can make of it: an object is not a value, it is
// a value with an identity, a prototype, a shape, an order of its own
// properties, and a lifetime that other code can measure.  Each case below is
// one of the ways identity becomes observable, written so that a rewrite which
// missed that way produces a *different printed line* rather than a crash.
//
//   POSITIVE cases (a correct optimizer changes these, and the output must not
//   move).  They exist because a pass that refuses everything is also wrong:
//   the output of each is a value that only survives if the fields were
//   forwarded, the slot was re-initialised per iteration, or the copy made by
//   the alias was tracked.  L is the case that caught a real defect -- a clear
//   emitted for an object whose literal is outside a loop but whose field is
//   read inside it, where the second iteration read the uninitialised marker.
//
//   NEGATIVE cases (the object must stay).  Each one makes the object's
//   identity observable in one specific way: it leaves the function, it is
//   asked a question only a real object can answer, or it is used by an
//   operation whose semantics this pass does not model.  All of them must
//   print the same thing before and after, and the way to tell whether the
//   rule is doing anything at all is to disable it and watch one of these
//   lines move.

function show(k, v) { print(k + '=' + v); }

/* ---- positive: fields that must be forwarded ---------------------------- */

function oneField(v) {
  var p = { x: v * 2 };
  return p.x + 1;
}
show('A1 one field', oneField(20));

function twoFields(a, b) {
  var p = { x: a, y: b };
  return p.x * 100 + p.y;
}
show('A2 two fields', twoFields(3, 7));

function overwritten(v) {
  var p = { x: 1, y: 2 };
  p.x = v;
  p.y = p.x + p.y;
  return p.x * 100 + p.y;
}
show('A3 overwrite', overwritten(5));

function repeatedRead(v) {
  var p = { x: v };
  return p.x + p.x + p.x;
}
show('A4 repeated read', repeatedRead(4));

function aliased(v) {
  var p = { x: v + 1 };
  var q = p;
  q.x = q.x * 2;
  return p.x;
}
show('A5 alias through a local', aliased(10));

function stackAlias(v) {
  var p = { x: v };
  p.x = p.x + 1;
  return p.x;
}
show('A6 alias on the stack', stackAlias(9));

function perIteration(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var p = { x: i, y: i + 1 };
    t += p.x + p.y;
  }
  return t;
}
show('A7 loop-local object', perIteration(5));

function branchSame(c, v) {
  var p = { x: 0 };
  if (c) { p.x = v; } else { p.x = v; }
  return p.x;
}
show('A8 branch, same value', branchSame(1, 6) + ',' + branchSame(0, 6));

function branchDifferent(c, v) {
  var p = { x: 0 };
  if (c) { p.x = v; } else { p.x = v * 10; }
  return p.x;
}
show('A9 branch, different values', branchDifferent(1, 6) + ',' + branchDifferent(0, 6));

var sideEffects = 0;
function effectful() { sideEffects++; return 3; }
function deadInitializer() {
  var p = { x: effectful(), y: 0 };
  return p.y;
}
show('A10 effectful initializer', deadInitializer() + ',' + sideEffects);

function everyType() {
  var out = '';
  var p = { i: 2147483647, b: true, n: null, u: undefined };
  out += p.i + ',' + p.b + ',' + p.n + ',' + p.u;
  var q = { s: 'text' };
  q.s = q.s + '!';
  return out + ',' + q.s;
}
show('A11 every field type', everyType());

function literalOutsideLoop(n) {
  var rhs = { u: 'ab' };
  var acc = '';
  for (var i = 0; i < n; i++) acc += rhs.u;
  return acc.length + '|' + acc.charAt(2 * n - 1);
}
show('A12 literal outside a loop', literalOutsideLoop(4));

function neverRead(v) {
  var p = { x: v, y: 1 };
  p.x = v + 1;
  return p.y;
}
show('A13 field never read', neverRead(8));

function onlyStored(v) {
  var p = { x: v };
  return 1;
}
show('A14 object nothing reads', onlyStored(1));

/* ---- negative: identity becomes observable ------------------------------ */

function returned(v) {
  var p = { x: v };
  p.y = v + 1;
  return p;
}
var r = returned(2);
show('N1 returned', r.x + ',' + r.y);

var unknownCall = function (o) { return o.x; };
function passedToCall(v) {
  var p = { x: v };
  p.x = v + 1;
  return unknownCall(p);
}
show('N2 passed to a call', passedToCall(3));

var sink = null;
function storedGlobally(v) {
  var p = { x: v };
  p.x = v + 2;
  sink = p;
  return 1;
}
storedGlobally(4);
sink.x = 99;
show('N3 stored globally', sink.x);

var box = {};
function storedInObject(v) {
  var p = { x: v };
  box.inner = p;
  return p.x;
}
show('N4 stored in another object', storedInObject(5) + ',' + box.inner.x);

function capturedInClosure(v) {
  var p = { x: v };
  var read = function () { return p.x; };
  p.x = v * 3;
  return read();
}
show('N5 captured by a closure', capturedInClosure(6));

function computedName(v, k) {
  var p = { x: v };
  p.x = v + 1;
  return p[k];
}
show('N6 computed property', computedName(7, 'x'));

function withMethod(v) {
  var p = { x: v, get double() { return this.x * 2; } };
  p.x = v + 1;
  return p.double;
}
show('N7 accessor property', withMethod(8));

var sym = Symbol('k');
function symbolKey(v) {
  var p = { x: v };
  p[sym] = v + 1;
  return p.x + ',' + p[sym];
}
show('N8 symbol key', symbolKey(9));

function withProto(v) {
  var p = { __proto__: { inherited: 7 }, x: v };
  p.x = v + 1;
  return p.x + ',' + p.inherited;
}
show('N9 __proto__ in a literal', withProto(10));

function newField(v) {
  var p = { x: v };
  p.x = v + 1;
  p.z = v + 2;
  return p.x + ',' + p.z + ',' + Object.keys(p).length;
}
show('N10 a field the shape does not have', newField(11));

function deleted(v) {
  var p = { x: v, y: v };
  delete p.x;
  return ('x' in p) + ',' + p.y + ',' + Object.keys(p).length;
}
show('N11 delete', deleted(12));

function enumerated(v) {
  var p = { x: v, y: v + 1 };
  var keys = '';
  for (var k in p) keys += k;
  return keys + ',' + p.x;
}
show('N12 for in', enumerated(13));

function spread(v) {
  var p = { x: v, y: v + 1 };
  var q = { ...p };
  q.x = 0;
  return q.x + ',' + q.y + ',' + p.x;
}
show('N13 spread', spread(14));

function destructured(v) {
  var p = { x: v, y: v + 1 };
  var { x, y } = p;
  p.x = 0;
  return x + ',' + y + ',' + p.x;
}
show('N14 destructuring', destructured(15));

var other = { x: 16 };
function compared(v) {
  var p = { x: v };
  p.x = v;
  var same = p === other;
  var sameFields = Object.is(p.x, other.x);
  return same + ',' + sameFields;
}
show('N15 identity compared', compared(16));

function prototypeRead(v) {
  var p = { x: v };
  p.x = v + 1;
  var proto = Object.getPrototypeOf(p);
  return p.x + ',' + (proto === Object.prototype);
}
show('N16 prototype read', prototypeRead(17));

function shapeChanged(c, v) {
  var p = { x: v };
  if (c) { p.y = 1; } else { p.y = 1; }
  return p.y + ',' + Object.keys(p).length;
}
show('N17 shape-changing branch', shapeChanged(1, 18) + ',' + shapeChanged(0, 18));

function mergedPath(c, v) {
  var p = { x: v };
  if (c) { box.kept = p; }
  p.x = v + 1;
  return p.x;
}
show('N18 merge where one path escapes', mergedPath(1, 19) + ',' + mergedPath(0, 19) + ',' + box.kept.x);

function stringified(v) {
  var p = { x: v };
  p.x = v + 1;
  return JSON.stringify(p);
}
show('N19 converted to a string', stringified(20));

function arrayOfObjects(v) {
  var p = { x: v };
  var a = [p];
  p.x = v + 1;
  return a[0].x + ',' + p.x;
}
show('N20 stored in an array', arrayOfObjects(21));

function duplicate(v) {
  var p = { x: v, x: v + 1 };
  p.x = p.x * 1;
  return p.x + ',' + Object.keys(p).length;
}
show('N21 duplicate property', duplicate(22));

function integerKey(v) {
  var p = { 7: v };
  p[7] = v + 1;
  return p[7] + ',' + Object.keys(p).length;
}
show('N22 integer key', integerKey(23));

function manyFields(v) {
  var p = { a1: v, a2: v, a3: v, a4: v, a5: v, a6: v, a7: v, a8: v, a9: v,
            b1: v, b2: v, b3: v, b4: v, b5: v, b6: v, b7: v, b8: v };
  p.a1 = v + 1;
  return p.a1 + ',' + p.b8 + ',' + Object.keys(p).length;
}
show('N23 more fields than the shape holds', manyFields(24));

function withHandler(v) {
  var p = { x: v };
  try {
    p.x = v + 1;
    if (v > 0) throw new Error('x');
  } catch (e) {
    p.x = 0;
  }
  return p.x;
}
show('N24 exception control flow', withHandler(25));

function methodCall(v) {
  var p = { x: v, m: function () { return this.x; } };
  p.x = v + 1;
  return p.m();
}
show('N25 method call', methodCall(26));

function deleteThenRead(v) {
  var p = { x: v };
  p.x = v + 1;
  var before = p.x;
  delete p.x;
  return before + ',' + ('x' in p) + ',' + Object.keys(p).length;
}
show('N26 in after delete', deleteThenRead(27));

function nullishChain(v) {
  var p = { x: v };
  p.x = v + 1;
  return (p.x ?? 0) + (p.missing ?? 5);
}
show('N27 a field outside the shape read', nullishChain(28));

function templateUse(v) {
  var p = { x: v };
  p.x = v + 1;
  return `x=${p.x}`;
}
show('N28 template literal', templateUse(29));

/* ---- negative: an alias that only one path establishes ------------------ */

/* `q` names the object on one arm and a different object on the other. The
   object the two arms disagree about cannot be taken apart, because `q.x`
   after the merge is a write to one object on one path and to the other on
   the other -- and neither may be virtualized while the other is real.  The
   two functions below are the same shape with the arms in the other order, so
   that the order the pass happens to visit them in cannot decide it. */
function aliasAssignedInThen(v) {
  var p = { x: v };
  var q = { x: 100 };
  if (v > 0) { q = p; }
  q.x = q.x + 1;
  return p.x + ',' + q.x;
}
show('N29 alias in the then arm', aliasAssignedInThen(5) + ',' + aliasAssignedInThen(-5));

function aliasAssignedInElse(v) {
  var p = { x: v };
  var q = { x: 100 };
  if (v > 0) { p.x = p.x + 1; } else { q = p; }
  q.x = q.x + 1;
  return p.x + ',' + q.x;
}
show('N30 alias in the else arm', aliasAssignedInElse(5) + ',' + aliasAssignedInElse(-5));

function aliasInLoop(v) {
  var p = { x: v };
  var q = { x: 0 };
  for (var i = 0; i < 3; i++) {
    if (i === 1) { q = p; }
    q.x = q.x + 1;
  }
  return p.x + ',' + q.x;
}
show('N31 alias assigned inside a loop', aliasInLoop(7));

/* ---- reference-valued fields --------------------------------------------
 *
 * A field that holds an object, a string or a function is a value like any
 * other: the slot holds the reference and the store that overwrites it
 * releases the one it replaced. These cases check the values, which both
 * engines agree on. The *timing* of the release is qjs's alone -- it frees by
 * reference count and node collects -- so it is measured by the
 * `escape.reference-release` kernel, where both arms are qjs, and by the
 * probe in the pass's report, not here. */

function mkObj(n) { return { n: n }; }
function mkStr(n) { return 's' + n; }

function objectField(n) {
  var p = { held: mkObj(n) };
  var v = p.held.n;
  p = null;
  return v + ',' + p;
}
show('N32 object field', objectField(3));

function functionField(n) {
  var p = { held: function () { return n; } };
  var v = p.held();
  p = null;
  return v + ',' + p;
}
show('N33 function field', functionField(4));

function stringField(n) {
  var p = { held: mkStr(n) };
  var v = p.held.length;
  p = null;
  return v + ',' + p;
}
show('N34 string field', stringField(41));

function overwrittenReference(n) {
  var p = { held: mkObj(n) };
  p.held = mkObj(n + 1);
  var v = p.held.n;
  p = null;
  return v + ',' + p;
}
show('N35 overwritten reference field', overwrittenReference(5));

function mixedFields(n) {
  var p = { obj: mkObj(n), str: mkStr(n), fn: function () { return n * 2; } };
  var v = p.obj.n + p.str.length + p.fn();
  p.obj = mkObj(n + 1);
  var w = p.obj.n;
  p = null;
  return v + ',' + w + ',' + p;
}
show('N36 mixed reference fields', mixedFields(6));

function referencesInALoop(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var p = { held: mkObj(i) };
    var v = p.held.n;
    p = null;
    t += v;
  }
  return t;
}
show('N37 references in a loop', referencesInALoop(20));

/* The same shape with enough allocation per iteration that a collector has a
   reason to look inside the loop. */
function referencesUnderPressure(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var p = { held: mkObj(i), pad: mkStr(i) + mkStr(i) };
    var v = p.held.n + p.pad.length;
    p = null;
    t += v;
  }
  return t;
}
show('N38 references under GC pressure', referencesUnderPressure(2000));
