/*
 * lazy-ic-table.js — patch 0030, lazy allocation of JSFunctionBytecode::ic_table.
 *
 * WHAT THIS CORPUS IS FOR, and what it provably cannot do.
 *
 * The inline cache (patch 0004) is a PURE CACHE: a correct engine and an engine
 * with the cache switched off print byte-identical output. So no output diff can
 * ever prove the cache is still *working* — that is what the `ich/*` counters in
 * `tools/opprof/instrument.py --ichit` are for, and the mutant that makes
 * `ic_table_ensure()` always return NULL is deliberately invisible here.
 *
 * What this corpus CAN prove is that making the table lazy did not make the
 * cache return WRONG answers or crash. Under lazy allocation `b->ic_table` is
 * NULL on the very first execution of every field-access site in every
 * function, so every case below runs its first iteration through a code path
 * that did not exist before this patch.
 *
 * Each block therefore drives one thing the IC has to get right *across* the
 * moment the table appears:
 *
 *  1  monomorphic own-property read/write, first call and steady state
 *  2  the borrow fusion `OP_get_loc_field_nr` (local receiver + field)
 *  3  prototype-chain reads (JS_IC_FLAG_PROTO) and what happens when the
 *     prototype's slot moves or gains an interceptor after the fill
 *  4  add-transitions (JS_IC_FLAG_TRANSITION) including a property-array grow,
 *     and the prototype watchpoint that must invalidate them
 *  5  polymorphism past capacity, i.e. the miss counter and the sticky
 *     MEGAMORPHIC bit, which live in the table and must survive it being
 *     allocated late
 *  6  shapes the IC refuses to cache (non-extensible, frozen, exotic, Proxy)
 *  7  a function that is compiled and never called, then called much later —
 *     the whole point of the patch
 *  8  delete / redefine / accessor conversion under a warmed site
 *
 * Deterministic, ES5, `print()` only.
 */

/* ---- 1. monomorphic own read/write, cold then warm ------------------- */
function Point(x, y) { this.x = x; this.y = y; }
function sumPoints(n) {
  var t = 0;
  for (var i = 0; i < n; i++) {
    var p = new Point(i, i + 1);
    p.x = p.x + 1;
    t += p.x + p.y;
  }
  return t;
}
print('1a ' + sumPoints(1));    /* first call: ic_table is NULL here */
print('1b ' + sumPoints(1));
print('1c ' + sumPoints(50));

/* the same site reached with a differently-shaped object after warming */
function readX(o) { return o.x; }
print('1d ' + readX({ x: 1 }) + ' ' + readX({ y: 2, x: 3 }) + ' ' +
      readX({ a: 0, b: 0, x: 4 }) + ' ' + readX({ x: 5 }));

/* ---- 2. the borrow fusion: receiver in a local ----------------------- */
function fused(n) {
  var o = { a: 1, b: 2, c: 3 };
  var t = 0;
  for (var i = 0; i < n; i++) t += o.a + o.b + o.c;
  return t;
}
print('2a ' + fused(1));
print('2b ' + fused(20));

/* ---- 3. prototype-chain reads --------------------------------------- */
function Base() {}
Base.prototype.kind = 'base';
Base.prototype.tag = 'T';
function Derived() { this.own = 1; }
Derived.prototype = new Base();
function protoRead(o) { return o.kind + '/' + o.tag; }
var d1 = new Derived();
print('3a ' + protoRead(d1));
print('3b ' + protoRead(d1));
/* move the cached slot on the prototype by adding properties before it */
var protoObj = Object.getPrototypeOf(Object.getPrototypeOf(d1));
protoObj.zzz1 = 1; protoObj.zzz2 = 2;
print('3c ' + protoRead(d1));
/* shadow the prototype property with an own one */
d1.kind = 'own';
print('3d ' + protoRead(d1));
/* turn a warmed prototype data property into an accessor */
function protoRead2(o) { return o.kind2; }
function B2() {}
B2.prototype.kind2 = 'plain';
var b2 = new B2();
print('3e ' + protoRead2(b2) + protoRead2(b2) + protoRead2(b2));
Object.defineProperty(B2.prototype, 'kind2', { get: function () { return 'getter'; }, configurable: true });
print('3f ' + protoRead2(b2) + protoRead2(b2));
/* and back to a data property */
Object.defineProperty(B2.prototype, 'kind2', { value: 'data', writable: true, configurable: true });
print('3g ' + protoRead2(b2) + protoRead2(b2));

/* ---- 4. add-transitions, property-array growth, watchpoints ---------- */
function build(n) {
  var o = {};
  o.p0 = n; o.p1 = n; o.p2 = n; o.p3 = n; o.p4 = n;
  o.p5 = n; o.p6 = n; o.p7 = n; o.p8 = n; o.p9 = n;
  return o.p0 + o.p9 + o.p5;
}
print('4a ' + build(1));
print('4b ' + build(2));
print('4c ' + build(3));

/* a warmed add-transition site whose prototype then gains a setter for the
   same key: the transition entry must stop hitting (watchpoint epoch) */
function Holder() {}
function addQ(o, v) { o.q = v; return o.q; }
print('4d ' + addQ(new Holder(), 1) + addQ(new Holder(), 2) + addQ(new Holder(), 3));
var seen = [];
Object.defineProperty(Holder.prototype, 'q', {
  set: function (v) { seen.push(v); }, get: function () { return 'S' + seen.length; },
  configurable: true
});
print('4e ' + addQ(new Holder(), 9) + ' seen=' + seen.join(','));
delete Holder.prototype.q;
print('4f ' + addQ(new Holder(), 4));

/* a warmed add-transition site whose prototype gains a NON-WRITABLE data
   property for the same key: the store must be refused in strict mode */
function Holder2() {}
function addR(o, v) { 'use strict'; o.r = v; return o.r; }
print('4g ' + addR(new Holder2(), 1) + addR(new Holder2(), 2) + addR(new Holder2(), 3));
Object.defineProperty(Holder2.prototype, 'r', { value: 'RO', writable: false, configurable: true });
try { print('4h ' + addR(new Holder2(), 5)); }
catch (e) { print('4h threw ' + (e instanceof TypeError)); }

/* ---- 5. polymorphism past capacity, then megamorphic ---------------- */
function poly(o) { return o.v; }
var shapes = [
  { v: 1 }, { a: 0, v: 2 }, { a: 0, b: 0, v: 3 }, { a: 0, b: 0, c: 0, v: 4 },
  { a: 0, b: 0, c: 0, d: 0, v: 5 }, { a: 0, b: 0, c: 0, d: 0, e: 0, v: 6 },
  { a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, v: 7 },
  { a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, v: 8 },
  { a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, v: 9 },
  { a: 0, b: 0, c: 0, d: 0, e: 0, f: 0, g: 0, h: 0, i: 0, v: 10 }
];
var acc = 0;
for (var r = 0; r < 6; r++) for (var s = 0; s < shapes.length; s++) acc += poly(shapes[s]);
print('5a ' + acc);
/* after going megamorphic the site must still read correctly, including a
   shape it saw before it gave up */
print('5b ' + poly(shapes[0]) + ' ' + poly(shapes[9]) + ' ' + poly({ v: 'late' }));

/* ---- 6. shapes the cache must refuse -------------------------------- */
var frozen = Object.freeze({ x: 'F' });
var sealed = Object.seal({ x: 'S' });
var noExt = Object.preventExtensions({ x: 'N' });
print('6a ' + readX(frozen) + readX(sealed) + readX(noExt));
var arr = [1, 2, 3];
arr.x = 'A';
print('6b ' + readX(arr) + ' ' + arr.length);
var withGetter = { get x() { return 'G'; } };
print('6c ' + readX(withGetter) + readX(withGetter));
if (typeof Proxy === 'function') {
  var pr = new Proxy({ x: 'P' }, { get: function (t, k) { return k === 'x' ? 'PX' : t[k]; } });
  print('6d ' + readX(pr) + readX(pr) + readX({ x: 'plain' }));
} else {
  print('6d PXPXplain');
}
/* a dictionary-mode / non-hashed shape: many deletes */
var dict = {};
for (var k = 0; k < 40; k++) dict['k' + k] = k;
for (var k2 = 0; k2 < 39; k2++) delete dict['k' + k2];
dict.x = 'D';
print('6e ' + readX(dict) + readX(dict));

/* ---- 7. a function compiled early and first called late -------------- */
function neverUntilNow(o) { return o.late + '|' + o.late2; }
function alsoNever(o) { o.w = 1; return o.w; }
var junk = 0;
for (var w = 0; w < 200; w++) junk += sumPoints(1);   /* churn other sites */
print('7a ' + neverUntilNow({ late: 'L', late2: 'M' }));
print('7b ' + neverUntilNow({ late: 'L', late2: 'M' }));
print('7c ' + alsoNever({}) + alsoNever({ z: 0 }));
print('7d ' + (junk > 0));

/* ---- 8. delete / redefine under a warmed site ------------------------ */
function slot(o) { return o.s; }
var o8 = { a: 1, s: 'first', b: 2 };
print('8a ' + slot(o8) + slot(o8));
delete o8.a;                       /* moves s */
print('8b ' + slot(o8) + slot(o8));
o8.s = 'second';
print('8c ' + slot(o8));
Object.defineProperty(o8, 's', { get: function () { return 'acc'; }, configurable: true });
print('8d ' + slot(o8) + slot(o8));
Object.defineProperty(o8, 's', { value: 'back', writable: true, configurable: true });
print('8e ' + slot(o8) + slot(o8));

/* ---- 9. closures and recursion: one bytecode, many frames ------------ */
function makeCounter(start) {
  var n = start;
  return function (o) { n += o.step; return n; };
}
var c1 = makeCounter(0), c2 = makeCounter(100);
print('9a ' + c1({ step: 1 }) + ' ' + c2({ step: 2 }) + ' ' +
      c1({ step: 3 }) + ' ' + c2({ step: 4 }));

function deep(o, n) { return n === 0 ? o.v : deep(o, n - 1) + 0; }
print('9b ' + deep({ v: 7 }, 30));

/* ---- 10. getters/setters defined via classes ------------------------- */
function Temp(c) { this.c = c; }
Object.defineProperty(Temp.prototype, 'f', {
  get: function () { return this.c * 9 / 5 + 32; },
  set: function (v) { this.c = (v - 32) * 5 / 9; },
  configurable: true
});
var t1 = new Temp(100);
print('10a ' + t1.f + ' ' + t1.f);
t1.f = 32;
print('10b ' + t1.c + ' ' + t1.f);
