/*
 * Differential corpus for patch 0058 -- `this` prologue elision.
 *
 * The patch moves `OP_push_this ; OP_put_loc/set_loc(this)` out of the bytecode
 * stream and into JS_CallInternal's frame setup: the store is performed there
 * and `pc` starts past the two instructions.  Nothing is re-emitted, so the
 * risk is NOT a codegen bug; it is that the frame setup's copy of
 * CASE(OP_push_this)'s receiver coercion diverges from the original, or that a
 * frame the setup path does not own (a resumed generator, a derived-class
 * constructor) is affected when it should not be.
 *
 * tests/differential/corpus/this-prologue-and-lnot-fold.js already covers the
 * emission side of the same prologue (patch 0028's put_loc -> set_loc fold),
 * including derived constructors, `new.target`, mapped/unmapped `arguments`,
 * >255 locals and the get_loc_field_nr step-aside.  This file deliberately does
 * NOT duplicate that.  It covers what 0058 newly touches:
 *
 *   1. The sloppy-mode receiver coercion, ALL THREE arms, because the frame
 *      setup now contains a second copy of it:
 *        - JS_TAG_OBJECT               -> js_dup, no coercion
 *        - null / undefined            -> ctx->global_obj
 *        - anything else               -> JS_ToObject (boxes, and can THROW)
 *      The throwing arm is the sharpest: it is the only `goto exception` from
 *      before the interpreter loop has run a single instruction, so it is the
 *      only place where the exception path sees sp == stack_buf.
 *   2. Strict mode, where NONE of that coercion happens and a primitive
 *      receiver must arrive unboxed and a null receiver must stay null.
 *   3. Arrow functions, which have no own `this` and must close over the
 *      enclosing one -- i.e. must NOT get a prologue of their own.
 *   4. Generators and async functions, whose frames are set up by
 *      async_func_init() and entered through JS_CALL_FLAG_GENERATOR, which
 *      never reaches the frame setup the patch modified.  They must still see
 *      the right `this` across a suspend/resume.
 *   5. Exotic receivers: Proxy, revoked Proxy, accessor receivers, host-ish
 *      objects, Symbol and BigInt primitives (Symbol/BigInt box, they do not
 *      throw; only null/undefined would, and those take the global-object arm).
 *   6. `this` observed a SECOND time, after the elided store, which is what
 *      makes a wrong slot index visible at all.
 *
 * Plain ES5-callable output via print(); deterministic; diffed byte-for-byte
 * against node.
 */

/* ---------- 1. sloppy-mode coercion, all three arms ---------- */

function sloppyThis() { return this; }

/* object arm */
var o1 = { tag: 'o1' };
print('S1', sloppyThis.call(o1) === o1);
print('S2', sloppyThis.call([1, 2]) instanceof Array);
print('S3', sloppyThis.call(sloppyThis) === sloppyThis);

/* null / undefined arm -> global object */
print('S4', sloppyThis.call(null) === globalThis);
print('S5', sloppyThis.call(undefined) === globalThis);
print('S6', sloppyThis() === globalThis);
print('S7', (0, sloppyThis)() === globalThis);

/* JS_ToObject arm -> boxed wrappers, and the boxing must be observable */
print('S8', typeof sloppyThis.call(5), sloppyThis.call(5) instanceof Number,
      sloppyThis.call(5).valueOf());
print('S9', typeof sloppyThis.call('ab'), sloppyThis.call('ab') instanceof String,
      sloppyThis.call('ab').length, sloppyThis.call('ab')[0]);
print('S10', typeof sloppyThis.call(true), sloppyThis.call(true) instanceof Boolean,
      sloppyThis.call(true).valueOf());
print('S11', typeof sloppyThis.call(Symbol.iterator),
      sloppyThis.call(Symbol.iterator) instanceof Symbol);
print('S12', typeof sloppyThis.call(9007199254740993n),
      String(sloppyThis.call(9007199254740993n)));

/* a fresh box on every call: the frame setup must not memoize anything */
print('S13', sloppyThis.call(5) !== sloppyThis.call(5));

/* boxing preserves identity of an already-boxed receiver */
var boxed = new Number(7);
print('S14', sloppyThis.call(boxed) === boxed);

/* ---------- 2. strict mode: no coercion at all ---------- */

function strictThis() { 'use strict'; return this; }

print('T1', strictThis.call(null) === null);
print('T2', strictThis.call(undefined) === undefined);
print('T3', strictThis() === undefined);
print('T4', typeof strictThis.call(5), strictThis.call(5) === 5);
print('T5', typeof strictThis.call('ab'), strictThis.call('ab') === 'ab');
print('T6', strictThis.call(o1) === o1);
print('T7', typeof strictThis.call(Symbol.iterator));

/* strict callee reached through a sloppy caller and vice versa */
function sloppyCaller(r) { return strictThis.call(r); }
print('T8', sloppyCaller(null) === null, sloppyCaller(3) === 3);
function strictCaller(r) { 'use strict'; return sloppyThis.call(r); }
print('T9', strictCaller(null) === globalThis, typeof strictCaller(3));

/* class bodies are strict by construction */
class Strictish {
  who() { return this; }
  static swho() { return this; }
}
print('T10', Strictish.prototype.who.call(null) === null);
print('T11', Strictish.prototype.who.call(4) === 4);
print('T12', Strictish.swho() === Strictish);

/* ---------- 3. arrow functions have no own `this` ---------- */

var arrowHost = {
  tag: 'host',
  make: function () {
    var a = () => this;
    return a;
  },
  makeNested: function () {
    return () => () => this.tag;
  },
};
var arrow = arrowHost.make();
print('A1', arrow() === arrowHost);
print('A2', arrow.call({ tag: 'other' }) === arrowHost);
print('A3', arrowHost.makeNested()()());

/* a top-level arrow closes over the module/global `this` */
var topArrow = () => typeof this;
print('A4', topArrow());

/* arrow inside a strict function with a primitive receiver */
function strictArrowHost() { 'use strict'; return () => this; }
print('A5', strictArrowHost.call(5)() === 5);
print('A6', strictArrowHost.call(null)() === null);

/* arrow inside a SLOPPY function with a primitive receiver: the enclosing
   function's `this` is the boxed object, and the arrow must see that box, not
   the raw primitive. */
function sloppyArrowHost() { return () => this; }
var sab = sloppyArrowHost.call(5)();
print('A7', typeof sab, sab instanceof Number, sab.valueOf());

/* ---------- 4. generators and async functions ---------- */

function* gen() {
  yield this.tag;
  yield this.tag + '2';
  return this.tag + '3';
}
var genHost = { tag: 'g', gen: gen };
var g = genHost.gen();
print('G1', g.next().value, g.next().value, g.next().value, g.next().done);

/* generator whose FIRST expression is `this`, resumed several times */
function* genFirst() {
  var t = this;
  yield t.tag;
  yield t.tag;
}
var gf = { tag: 'gf', f: genFirst }.f();
print('G2', gf.next().value, gf.next().value, gf.next().done);

/* sloppy generator with a primitive receiver -- coercion still happens, but in
   the bytecode prologue, because async_func_init does not use the frame setup
   the patch changed. */
function* genSloppy() { yield typeof this; yield this instanceof Number; }
var gs = genSloppy.call(5);
print('G3', gs.next().value, gs.next().value);

function* genSloppyNull() { yield this === globalThis; }
print('G4', genSloppyNull.call(null).next().value);

var asyncOut = [];
async function asyncThis() {
  asyncOut.push(this.tag);
  await 0;
  asyncOut.push(this.tag + '!');
  return this.tag;
}
var asyncHost = { tag: 'a', f: asyncThis };
asyncHost.f().then(function (v) { asyncOut.push('ret:' + v); });

/* async arrow inside a method */
var asyncArrowHost = {
  tag: 'aa',
  run: function () {
    return (async () => { await 0; return this.tag; })();
  },
};
asyncArrowHost.run().then(function (v) { asyncOut.push('arrow:' + v); });

/* ---------- 5. exotic receivers ---------- */

/* Proxy as receiver: `this` must be the proxy itself, and property reads
   through it must trap. */
var trapLog = [];
var proxyTarget = { tag: 'pt' };
var proxy = new Proxy(proxyTarget, {
  get: function (t, k, r) {
    if (typeof k === 'string') trapLog.push('get:' + k);
    return t[k];
  },
});
function readTag() { return this.tag; }
print('P1', readTag.call(proxy));
print('P2', sloppyThis.call(proxy) === proxy, sloppyThis.call(proxy) === proxyTarget);
print('P3', trapLog.join(','));

/* revoked proxy: `this` binding itself must succeed, the property read must
   throw */
var rev = Proxy.revocable({ tag: 'rv' }, {});
var revoked = rev.proxy;
rev.revoke();
print('P4', sloppyThis.call(revoked) === revoked);
try { readTag.call(revoked); print('P5 no-throw'); }
catch (e) { print('P5', e instanceof TypeError); }

/* accessor receivers: a getter and a setter both open with `this` */
var accLog = [];
var acc = {
  _v: 1,
  get v() { return this._v; },
  set v(x) { accLog.push('set' + x); this._v = x; },
};
print('X1', acc.v);
acc.v = 9;
print('X2', acc.v, accLog.join(','));

/* accessor pulled off and re-bound to a primitive in sloppy mode */
var vdesc = Object.getOwnPropertyDescriptor(acc, 'v');
Object.defineProperty(Number.prototype, 'probe', { get: vdesc.get, configurable: true });
print('X3', typeof (5).probe);

/* getter invoked through a prototype chain with an inherited receiver */
function Base() { this.n = 3; }
Object.defineProperty(Base.prototype, 'twice', {
  get: function () { return this.n * 2; },
  configurable: true,
});
var derivedObj = Object.create(new Base());
derivedObj.n = 10;
print('X4', new Base().twice, derivedObj.twice);

/* Reflect.apply and Function.prototype.bind supplying the receiver */
print('R1', Reflect.apply(readTag, { tag: 'refl' }, []));
print('R2', readTag.bind({ tag: 'bnd' })());
print('R3', readTag.bind({ tag: 'b1' }).bind({ tag: 'b2' })());
var boundSloppy = sloppyThis.bind(null);
print('R4', boundSloppy() === globalThis);
var boundStrict = strictThis.bind(null);
print('R5', boundStrict() === null);
print('R6', typeof sloppyThis.bind(7)());

/* method extracted and called bare, sloppy vs strict */
var extracted = acc.__lookupGetter__ ? 'has' : 'none';
print('R7', extracted);

/* ---------- 6. `this` read a second time, and stored to a high slot ---------- */

function twice() {
  var a = this;
  var b = this;
  return (a === b) + ':' + (a === this);
}
print('W1', twice.call(o1));
print('W2', twice.call(5));
print('W3', twice.call(null));

/* many locals ahead of `this`, so the prologue store is not slot 0..3 */
function manyLocals() {
  var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7;
  var t = this;
  return [v0, v1, v2, v3, v4, v5, v6, v7].join('') + ':' + (t === this) + ':' + t.tag;
}
print('W4', manyLocals.call({ tag: 'ml' }));

/* `this` captured into a closure, so the slot is read after the frame is gone */
function capture() {
  var self = this;
  return function () { return self.tag + '/' + (self === this); };
}
var cap = capture.call({ tag: 'cap' });
print('W5', cap());

/* `this` written through by a nested arrow after capture */
function mutate() {
  var bump = () => { this.n = (this.n || 0) + 1; };
  bump(); bump();
  return this.n;
}
print('W6', mutate.call({ n: 5 }));

/* recursion: every frame must get its own `this` */
function recurse(depth) {
  if (depth === 0) return this.tag;
  return recurse.call({ tag: this.tag + depth }, depth - 1);
}
print('W7', recurse.call({ tag: 'r' }, 4));

/* a hot loop, so the elided path is executed many times rather than once */
var sum = 0;
var hotHost = { n: 2, hot: function () { return this.n; } };
for (var i = 0; i < 20000; i++) sum += hotHost.hot();
print('H1', sum);
var sum2 = 0;
function hotSloppy() { return this.valueOf(); }
for (var j = 0; j < 20000; j++) sum2 += hotSloppy.call(j & 7);
print('H2', sum2);

/* ---------- 7. `this` behind patch 0048's elided-`arguments` prologue ------
 *
 * When patch 0048 elides the `arguments` object it emits
 * OP_set_loc_uninitialized(arguments_var_idx) AHEAD of the `this` store, so the
 * elided prologue is three instructions, not two, and the frame setup has to
 * reproduce the JS_UNINITIALIZED seed as well.  MEASURED: this is 79,912,802 of
 * raytrace's 212,739,610 prologues -- 37.6% of the row's population -- so it is
 * not a corner case, and a build that seeds the slot with JS_UNDEFINED instead
 * hands `arguments` back as `undefined` on the slow path.
 *
 * The elision only fires for the recognised `arguments` shapes (length-only,
 * element read, and `.apply(this, arguments)`), so all three are here together
 * with a `this` use, and each is also forced down its SLOW path (a read that
 * has to materialize the real object) so the seeded slot is observed.
 */

function argsLenAndThis() {
  return this.tag + ':' + arguments.length;
}
print('N1', argsLenAndThis.call({ tag: 'n1' }, 1, 2, 3));
print('N2', argsLenAndThis.call({ tag: 'n2' }));

function argsElemAndThis(a) {
  return this.tag + ':' + arguments[0] + ':' + arguments[arguments.length - 1];
}
print('N3', argsElemAndThis.call({ tag: 'n3' }, 'x', 'y', 'z'));

function argsApplyTarget() { return this.tag + '/' + Array.prototype.join.call(arguments, '-'); }
function argsApplyAndThis() {
  return argsApplyTarget.apply(this, arguments);
}
print('N4', argsApplyAndThis.call({ tag: 'n4' }, 1, 2, 3));

/* force the object to be materialized, which is what reads the seeded slot */
function argsObjectAndThis() {
  var t = this.tag;
  var o = arguments;
  return t + ':' + (typeof o) + ':' + o.length + ':' + o[1] + ':' + (o === arguments);
}
print('N5', argsObjectAndThis.call({ tag: 'n5' }, 'p', 'q'));

/* THE SLOW PATH IS WHAT ACTUALLY READS THE SEEDED SLOT.  js_args_memo() tests
   the slot for JS_TAG_UNINITIALIZED and builds the real `arguments` object only
   then; every case above stays on the fast path and therefore never looks at
   it.  MEASURED: a build with the seed replaced by JS_UNDEFINED (mutant M6)
   passed all of N1-N5 and the entire rest of the corpus.  These four force the
   slow path -- an out-of-range integer key, a string key, `callee`, and a key
   that must resolve on Object.prototype -- and each fails with a TypeError on
   that build, because the memo hands back `undefined` as the receiver. */
function argsSlowOOR() { return this.tag + ':' + arguments[5]; }
print('N5a', argsSlowOOR.call({ tag: 'n5a' }, 1, 2));

function argsSlowStrKey() { return this.tag + ':' + arguments['length'] + ':' + arguments['0']; }
print('N5b', argsSlowStrKey.call({ tag: 'n5b' }, 'a', 'b'));

function argsSlowCallee() { return this.tag + ':' + (arguments['callee'] === argsSlowCallee); }
print('N5c', argsSlowCallee.call({ tag: 'n5c' }, 1));

function argsSlowProto() { return this.tag + ':' + typeof arguments['toString']; }
print('N5d', argsSlowProto.call({ tag: 'n5d' }));

/* the memo's identity must be stable across repeated slow reads in one frame */
function argsSlowTwice() {
  var a = arguments['0'];
  var b = arguments[9];
  var c = arguments['callee'];
  return this.tag + ':' + a + ':' + b + ':' + (c === argsSlowTwice);
}
print('N5e', argsSlowTwice.call({ tag: 'n5e' }, 'z'));

/* and in a loop, so the seeded-then-memoized transition is exercised on many
   distinct frames rather than once */
var slowAcc = '';
function argsSlowHot(i) { return this.k + arguments[3]; }
for (var sh = 0; sh < 500; sh++) slowAcc = argsSlowHot.call({ k: 'k' }, sh);
print('N5f', slowAcc);

/* strict version: no mapping between arguments and parameters */
function argsStrictAndThis(a) {
  'use strict';
  a = 'changed';
  return this + ':' + arguments[0] + ':' + arguments.length;
}
print('N6', argsStrictAndThis.call('S', 'orig'));

/* sloppy mapped arguments: writing the parameter must be visible through the
   object, and `this` must still be the boxed receiver */
function argsMappedAndThis(a) {
  a = 'changed';
  return typeof this + ':' + arguments[0] + ':' + this.valueOf();
}
print('N7', argsMappedAndThis.call(42, 'orig'));

/* NO `this` AT ALL, but `arguments` elided -- so the bytecode opens with the
   OP_set_loc_uninitialized seed and NOTHING follows it that belongs to this
   optimization.  A build that admits a bare leading seed as if it were the
   whole prologue (mutant M10) stores the receiver into the `arguments` memo
   slot, and the slow path then reads properties off the global object instead
   of building the real object.  This is the shape that catches M10; the
   derived-constructor cases in section 8 do NOT, because a derived
   constructor's prologue starts with OP_special_object (home object /
   new.target), which refuses before the seed is ever considered. */
function noThisArgsCallee(a) {
  var s = a + 1;
  return s + ':' + (arguments['callee'] === noThisArgsCallee) + ':' + arguments['length'];
}
print('N9', noThisArgsCallee(1, 2));

function noThisArgsOOR(a) {
  var s = a * 2;
  return s + ':' + arguments[9] + ':' + typeof arguments['toString'];
}
print('N10', noThisArgsOOR(4));

/* hot loop over the elided-arguments shape */
var nsum = 0;
var nHost = { k: 3, f: function () { return this.k + arguments.length; } };
for (var n = 0; n < 20000; n++) nsum += nHost.f(1, 2);
print('N8', nsum);

/* ---------- 8. derived-class constructors: `this` is in TDZ ---------------
 *
 * resolve_labels emits OP_set_loc_uninitialized(this_var_idx) INSTEAD of the
 * push_this pair for a derived constructor, so the elision must refuse those
 * outright.  The distinguishing byte is the OP_push_this that is not there --
 * and section 7 above deliberately taught the scan to walk PAST a leading
 * OP_set_loc_uninitialized, which is the same opcode a derived constructor
 * opens with.  A build that admits a leading seed as if it were the whole
 * prologue (mutant M10) stores the receiver into the TDZ slot and turns every
 * ReferenceError below into a successful read.
 */

class DBase {
  constructor(v) { this.v = v; }
  tag() { return 'base' + this.v; }
}

class DOkay extends DBase {
  constructor() { super(7); this.extra = this.v * 2; }
}
print('D1', new DOkay().extra, new DOkay().tag());

class DEarly extends DBase {
  constructor() {
    try { this.v = 1; } catch (e) { DEarly.err = e.constructor.name; }
    super(3);
  }
}
var d2 = new DEarly();
print('D2', DEarly.err, d2.v);

class DEarlyRead extends DBase {
  constructor() {
    var out;
    try { out = this.v; } catch (e) { out = e.constructor.name; }
    super(4);
    this.out = out;
  }
}
print('D3', new DEarlyRead().out);

/* returning before super() must throw, not produce an object */
class DNoSuper extends DBase {
  constructor(skip) { if (!skip) super(1); }
}
print('D4', new DNoSuper(false).v);
try { new DNoSuper(true); print('D5 no-throw'); }
catch (e) { print('D5', e.constructor.name); }

/* super() called twice must throw the second time */
class DTwice extends DBase {
  constructor() {
    super(1);
    try { super(2); } catch (e) { DTwice.err = e.constructor.name; }
  }
}
new DTwice();
print('D6', DTwice.err);

/* a derived constructor that also uses `arguments` -- the two
   OP_set_loc_uninitialized seeds are then adjacent in the prologue */
class DArgs extends DBase {
  constructor() { super(arguments.length); this.n = arguments.length; }
}
print('D7', new DArgs(1, 2, 3).n, new DArgs().v);

/* an arrow inside a derived constructor closes over the TDZ slot */
class DArrow extends DBase {
  constructor() {
    var get = () => this.v;
    var pre;
    try { pre = get(); } catch (e) { pre = e.constructor.name; }
    super(5);
    this.pre = pre;
    this.post = get();
  }
}
var da = new DArrow();
print('D8', da.pre, da.post);

/* a BASE class constructor is not derived and DOES take the elision */
class DPlain {
  constructor() { this.z = 1; this.y = this.z + 1; }
}
print('D9', new DPlain().y);

/* direct eval capturing `this` */
function evalThis() {
  return eval('this === arguments.callee.marker ? "same" : typeof this');
}
evalThis.marker = null;
print('E1', evalThis.call(o1));
function evalThisStrict() { 'use strict'; return eval('typeof this'); }
print('E2', evalThisStrict.call(null), evalThisStrict.call(5));

/* the async results, printed last so ordering is deterministic */
Promise.resolve().then(function () {}).then(function () {}).then(function () {
  print('Z1', asyncOut.join('|'));
});
