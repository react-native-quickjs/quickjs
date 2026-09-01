/*
 * Differential corpus for the two resolve_labels() peepholes found by the
 * Phase 0 bytecode census (docs/build-time-bytecode-optimizer.md):
 *
 *   A. push_i32(0) lnot -> push_true   /   push_i32(v != 0) lnot -> push_false
 *   B. push_this put_loc(this) get_loc(this) -> push_this set_loc(this)
 *
 * WHAT EACH CASE IS FOR. The two failure patterns recorded in
 * .claude/skills/rnqjs-patch/SKILL.md are (i) a corpus that reproduces the
 * assumption it exists to test and (ii) a corpus that never reaches the code.
 * Both are guarded against here deliberately:
 *
 *  - Fold B only fires when the FIRST instruction of the body reads the
 *    `this` slot, so a method that touches `this` late (B3 `late`) and a method
 *    that never touches it (B4 `never`) are included as negatives.
 *  - Fold B must NOT fire when the body's first instruction is a branch target
 *    (B5 `loopFirst`) or when another prologue initializer intervenes
 *    (B6/B7/B35/B36, `arguments` mapped and unmapped). Those are two of the
 *    three admission guards.
 *  - Fold B deliberately steps aside when the first instruction is
 *    `get_loc(this) get_field` or `get_loc(this) get_array_el`, so that patch
 *    0004's OP_get_loc_field_nr and patch 0020's OP_get_loc8_array_el keep
 *    firing (B1, B24). That is the third guard, and it is a PERFORMANCE guard:
 *    no output diff can see it, only the hit counter can.
 *  - Fold B rewrites put_loc{0..3,8,general} into the matching set_loc.
 *    MEASURED: `this` lands in slot 1, not slot 0, for an ordinary object
 *    method, so a mutant that hard-codes OP_set_loc0 stores the receiver into
 *    the wrong slot -- invisible unless `this` is read a SECOND time after the
 *    fold, which is what B31/B32/B37 exist for.
 *  - Fold A must respect the VALUE: !0 is true and !1 is false, so a mutant
 *    that ignores `val` flips half the answers. Negative and multi-digit
 *    constants are included because push_i32 is the only opcode matched --
 *    `!"" `, `!null` and `!undefined` go down other paths and must be
 *    unaffected.
 *
 * Every line is printed so a byte-for-byte diff against node discriminates.
 */

/* ---------- A: constant lnot folding ---------- */
print('A1', !0, !1, !2, !-1, !7, !123456789);
print('A2', !!0, !!1, !!!0);
print('A3', typeof !0, typeof !1);
print('A4', !0 === true, !1 === false, !0 == 1, !1 == 0);
print('A5', String(!0) + String(!1));
print('A6', [!0, !1].join(','), JSON.stringify({ a: !0, b: !1 }));
/* the value must survive a branch, both senses */
if (!0) print('A7 taken'); else print('A7 not-taken');
if (!1) print('A8 taken'); else print('A8 not-taken');
print('A9', !0 ? 'y' : 'n', !1 ? 'y' : 'n');
/* non-int32 operands take other opcodes and must be unchanged */
print('A10', !'', !'x', !null, !undefined, !NaN, ![], !{});
/* -0 and 0.5 are push_const, not push_i32 */
print('A11', !-0, !0.5, !0.0);
/* lnot on a value the optimizer cannot know */
var av = [0, 1, 2][0];
print('A12', !av, !(av + 1));
/* short-circuit contexts */
print('A13', (!0 && 'and'), (!1 || 'or'), (!0 || 'never'), (!1 && 'never'));
/* in a loop, so the folded push is executed many times */
var acc = 0;
for (var i = 0; i < 5; i++) acc += !0 ? 1 : 0;
for (var j = 0; j < 5; j++) acc += !1 ? 100 : 0;
print('A14', acc);

/* ---------- B: the `this` prologue ---------- */

/* B1: body's FIRST instruction reads `this` -- the fold fires. */
var b1 = {
  x: 11,
  first: function () {
    return this.x;
  },
};
print('B1', b1.first());

/* B2: `this` read first, then the slot is read again several times.
   set_loc must actually STORE, not merely leave the value on the stack. */
var b2 = {
  x: 3,
  y: 4,
  sum: function () {
    var a = this.x;
    var b = this.y;
    return a + b + this.x + this.y;
  },
};
print('B2', b2.sum());

/* B3: NEGATIVE -- `this` is touched only after other work. A mutant that
   folds unconditionally leaves the receiver on the stack here. */
var b3 = {
  x: 5,
  late: function (p) {
    var t = p + 1;
    var u = t * 2;
    return u + this.x;
  },
};
print('B3', b3.late(9));

/* B4: NEGATIVE -- `this` never read at all. */
var b4 = {
  never: function (p) {
    return p * 2;
  },
};
print('B4', b4.never(21));

/* B5: the body's first instruction is a BRANCH TARGET (a loop header).
   code_match() must refuse to look through the OP_label. */
var b5 = {
  n: 4,
  loopFirst: function () {
    var s = 0;
    while (this.n > 0) {
      s += this.n;
      this.n--;
    }
    return s;
  },
};
print('B5', b5.loopFirst());

/* B6: `arguments` -- another prologue initializer is emitted after the
   `this` store, so the `bc_out.size == this_put_end` guard must reject. */
var b6 = {
  x: 6,
  withArgs: function (a, b) {
    return this.x + arguments.length + arguments[0] + a + b;
  },
};
print('B6', b6.withArgs(1, 2));

/* B7: strict-mode `arguments` (unmapped) takes the other branch. */
var b7 = {
  x: 7,
  strictArgs: function (a) {
    'use strict';
    return this.x + arguments.length + a;
  },
};
print('B7', b7.strictArgs(2));

/* B8: `this` captured by a closure -- the slot is boxed into a JSVarRef, so
   the store must be observable from the closure. */
var b8 = {
  x: 8,
  capture: function () {
    var self = this.x;
    var f = function () {
      return self;
    };
    return f() + this.x;
  },
};
print('B8', b8.capture());

/* B9: arrow function inheriting `this` from an enclosing method. */
var b9 = {
  x: 9,
  arrow: function () {
    var g = () => this.x + 1;
    return g();
  },
};
print('B9', b9.arrow());

/* B10: slot index above 3 (so put_loc8 / set_loc8) and above 255
   (so the general 3-byte put_loc / set_loc). `this` is declared first, but
   many other locals exist; the index the compiler picks is what matters. */
var b10 = {
  x: 10,
  manyLocals: function () {
    var v0 = this.x,
      v1 = 1,
      v2 = 2,
      v3 = 3,
      v4 = 4,
      v5 = 5,
      v6 = 6,
      v7 = 7;
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7;
  },
};
print('B10', b10.manyLocals());

/* B11: a class method and a plain constructor. */
class C11 {
  constructor(v) {
    this.v = v;
  }
  get double() {
    return this.v * 2;
  }
  triple() {
    return this.v * 3;
  }
}
var c11 = new C11(4);
print('B11', c11.v, c11.double, c11.triple());

/* B12: DERIVED class constructor -- `this` starts uninitialized, so the
   prologue emits OP_set_loc_uninitialized and the fold must not apply. */
class D12 extends C11 {
  constructor(v) {
    super(v + 1);
    this.w = v;
  }
  both() {
    return this.v + this.w;
  }
}
var d12 = new D12(5);
print('B12', d12.v, d12.w, d12.both());

/* B13: `this` in sloppy mode at top level of a function called plainly --
   the receiver is the global object (or undefined in strict). */
function b13Sloppy() {
  return typeof this;
}
function b13Strict() {
  'use strict';
  return typeof this;
}
print('B13', b13Sloppy(), b13Strict());

/* B14: generator and async-shaped bodies opening with `this`. */
var b14 = {
  x: 14,
  *gen() {
    yield this.x;
    yield this.x + 1;
  },
};
var g14 = b14.gen();
print('B14', g14.next().value, g14.next().value, g14.next().done);

/* B15: method invoked with call/apply/bind, so the receiver varies. */
var b15 = {
  read: function () {
    return this.q;
  },
};
print('B15', b15.read.call({ q: 'a' }), b15.read.apply({ q: 'b' }), b15.read.bind({ q: 'c' })());

/* B16: `this` first AND the very next thing is a property STORE, which is the
   `push_this put_loc get_loc_field_nr` shape the census found on Octane. */
var b16 = {
  init: function (a, b) {
    this.a = a;
    this.b = b;
    return this.a + this.b;
  },
};
print('B16', b16.init(30, 12));

/* B17: throwing out of a method whose prologue was folded -- the operand
   stack is unwound with an extra live value if the rewrite is wrong. */
var b17 = {
  x: 17,
  boom: function () {
    var v = this.x;
    if (v) throw new Error('boom' + v);
    return v;
  },
};
try {
  b17.boom();
} catch (e) {
  print('B17', e.message);
}

/* B18: recursion, so the folded prologue runs at many stack depths. */
var b18 = {
  n: 0,
  down: function (k) {
    if (k <= 0) return this.n;
    this.n += k;
    return this.down(k - 1);
  },
};
print('B18', b18.down(10));

/* B19: both folds in one function. */
var b19 = {
  flag: function () {
    return this.on === !0 ? !1 : !0;
  },
  on: true,
};
print('B19', b19.flag());

/* ---------- B (continued): bodies whose FIRST use of `this` is NOT a field
 * read.  This is the subset the fold actually applies to: when the body opens
 * with `this.x` the get_loc is instead consumed by patch 0004's
 * get_loc_field_nr borrow-fusion, and the prologue fold deliberately steps
 * aside so as not to defeat it.  Without these cases the corpus reaches the
 * fold only twice and proves almost nothing. ---------- */

/* B20: `return this` -- the receiver itself is the value. */
var b20 = {
  self: function () {
    return this;
  },
};
print('B20', b20.self() === b20, b20.self().self() === b20);

/* B21: `this` aliased into a local first (the `var self = this` idiom). */
var b21 = {
  x: 21,
  alias: function () {
    var self = this;
    return function () {
      return self.x;
    };
  },
};
print('B21', b21.alias()());

/* B22: `this` passed as an argument before anything else happens. */
function idf(v) {
  return v && v.x;
}
var b22 = {
  x: 22,
  pass: function () {
    return idf(this);
  },
};
print('B22', b22.pass());

/* B23: `this` compared, so the first op after get_loc is a comparison. */
var b23 = {
  cmp: function (o) {
    return this === o;
  },
};
print('B23', b23.cmp(b23), b23.cmp({}));

/* B24: computed member access -- get_array_el, the OTHER fusion partner the
   fold steps aside for. */
var b24 = {
  x: 24,
  computed: function (k) {
    return this[k];
  },
};
print('B24', b24.computed('x'));

/* B25: `typeof this` and `this` in a template / concatenation. */
var b25 = {
  kind: function () {
    return typeof this;
  },
  str: function () {
    return '' + (this === b25);
  },
};
print('B25', b25.kind(), b25.str());

/* B26: slot index forced above 3 with `this` read first as a bare value. */
var b26 = {
  x: 26,
  many: function (p) {
    var a = 1,
      b = 2,
      c = 3,
      d = 4,
      e = 5,
      f = 6,
      g = 7,
      h = 8;
    var me = this;
    return a + b + c + d + e + f + g + h + p + me.x;
  },
};
print('B26', b26.many(0));

/* B27: recursion + `this` as a bare value, so the folded set_loc runs deep. */
var b27 = {
  n: 3,
  walk: function (k) {
    var me = this;
    if (k <= 0) return me.n;
    return me.walk(k - 1) + 1;
  },
};
print('B27', b27.walk(5));

/* B28: throwing after the folded prologue. */
var b28 = {
  boom: function () {
    var me = this;
    throw new Error('x' + (me === b28));
  },
};
try {
  b28.boom();
} catch (e2) {
  print('B28', e2.message);
}

/* B29: class method whose first `this` use is a bare value, plus a getter. */
class C29 {
  constructor() {
    this.v = 29;
  }
  me() {
    var m = this;
    return m.v;
  }
  get id() {
    var m = this;
    return m.v + 1;
  }
}
var c29 = new C29();
print('B29', c29.me(), c29.id);

/* B30: generator whose body opens with a bare `this`. */
var b30 = {
  v: 30,
  *g() {
    var m = this;
    yield m.v;
    yield m.v + 1;
  },
};
var it30 = b30.g();
print('B30', it30.next().value, it30.next().value);

/* ---------- B (continued): cases written specifically to KILL mutants.
 * Added after a mutation run in which four deliberately broken builds passed
 * everything above. Each case names the mutant it discriminates. ---------- */

/* B31: kills "always emit OP_set_loc0".  MEASURED: `this` lands in slot 1 (not
 * 0) for an ordinary object method, so a mutant that hard-codes set_loc0 stores
 * the receiver into the WRONG slot.  That is only observable if `this` is read
 * AGAIN after the folded prologue -- the first read takes its value off the
 * stack and would be right either way. */
var b31 = {
  x: 31,
  twice: function () {
    var me = this;
    var w = 100;
    return me.x + this.x + w;
  },
};
print('B31', b31.twice());

/* B32: same, with the second read going through a different path (computed). */
var b32 = {
  x: 32,
  again: function (k) {
    var me = this;
    return me.x + this[k];
  },
};
print('B32', b32.again('x'));

/* B33: kills "ignore the local index in the code_match".  The body's FIRST
 * instruction is a get_loc of a DIFFERENT local (an uninitialised `var`), so a
 * mutant matching any get_loc deletes that read and substitutes the receiver. */
var b33 = {
  x: 33,
  otherFirst: function () {
    var a;
    var t = a;
    return (t === undefined) + '/' + this.x;
  },
};
print('B33', b33.otherFirst());

/* B34: same shape but the misappropriated local is numeric, so the wrong
 * value propagates arithmetically rather than as a type change. */
var b34 = {
  x: 34,
  otherFirstNum: function () {
    var a = void 0;
    var t = a;
    var me = this;
    return String(t) + ':' + me.x;
  },
};
print('B34', b34.otherFirstNum());

/* B35: `arguments` present AND the body opens with a bare `this` read, so the
 * `bc_out.size == this_put_end` guard is the only thing standing between the
 * fold and a prologue with a second initializer after the `this` store. */
var b35 = {
  x: 35,
  argsAndThis: function (a, b) {
    var me = this;
    return me.x + arguments.length + arguments[0] + a + b;
  },
};
print('B35', b35.argsAndThis(1, 2));

/* B36: strict-mode variant of B35 (unmapped arguments object). */
var b36 = {
  x: 36,
  argsStrict: function (a) {
    'use strict';
    var me = this;
    return me.x + arguments.length + a + (this === b36);
  },
};
print('B36', b36.argsStrict(4));

/* B37: named function expression, which occupies a slot ahead of `this`. */
var b37 = {
  x: 37,
  named: function nfe(k) {
    var me = this;
    if (k > 0) return nfe.call(me, k - 1);
    return me.x + this.x;
  },
};
print('B37', b37.named(2));

/* ---------- G: the GENERALISED prologue fold.
 *
 * The fold shipped as patch 0028 folds whichever prologue initializer emitted
 * the LAST OP_put_loc, not only `this`. The other two that occur in practice
 * are `arguments` and `new.target`; the remaining five (home object, active
 * function, function self-reference, and the two variable-environment objects)
 * are emitted before `this` or before `arguments` and so are only ever the last
 * one when neither of those exists.
 *
 * MEASURED, site counter (-DQJS_R3_STATS) over four large real programs:
 * `arguments` folds 8 times in lodash.js and once in Octane earley-boyer;
 * `new.target` and the other five fold zero times anywhere measured. The G
 * cases below therefore exist because the general form is *reachable*, not
 * because it is hot -- without them the `arguments` and `new.target` arms would
 * have no test at all. ---------- */

/* G1: body opens with a bare `arguments` read. `this` is never mentioned, so
   `arguments` is the last (and only) prologue put_loc. */
function g1() {
  var a = arguments;
  var n = a.length;
  return n + a[0] + a[n - 1];
}
print('G1', g1(1, 2, 3));

/* G2: NEGATIVE for the step-aside guard -- the body opens with
   `get_loc(arguments) get_field`, which patch 0004's borrow fusion wants. */
function g2() {
  var n = arguments.length;
  return n * 10;
}
print('G2', g2(1, 2, 3, 4));

/* G3: kills "always emit OP_set_loc0" and "sop = pop + 1" on the `arguments`
   arm: the slot is read AGAIN after the fold, so a store into the wrong slot
   is observable. Strict mode, so the arguments object is unmapped. */
function g3(p) {
  'use strict';
  var a = arguments;
  var q = p * 2;
  return a.length + q + arguments[0] + a[0];
}
print('G3', g3(7));

/* G4: sloppy mode, so `arguments` is MAPPED and aliases the parameters. The
   fold must not disturb that aliasing. */
function g4(p) {
  var a = arguments;
  p = 99;
  return a[0] + arguments[0] + p;
}
print('G4', g4(1));

/* G5: `new.target` read as the first thing in the body, both when constructed
   and when called plainly. */
function G5() {
  var t = new.target;
  return t === undefined ? 'plain' : 'ctor:' + t.name;
}
print('G5', G5(), new G5() instanceof G5);

/* G6: `new.target` AND `this`, so `this` is the last prologue put_loc and the
   body opens with `new.target` -- the fold must NOT fire (different index),
   and both bindings must still be correct. */
function G6() {
  var t = new.target;
  var me = this;
  me.r = (t === undefined ? 'plain' : 'ctor') + '/' + typeof me;
}
var g6o = {};
G6.call(g6o);
print('G6', new G6().r, g6o.r);

/* G7: the general 3-byte OP_put_loc / OP_set_loc form. `this` is allocated a
   local slot lazily by add_var_this() at scope-resolution time, so it takes the
   NEXT sequential slot -- a function with more than 255 locals pushes it past
   the byte-indexed short forms. MEASURED with the site counter: this case is
   the only one in the corpus that reaches that branch, and it does reach it.
   The function is built with eval() so the local count is explicit. */
var g7src = '(function () { var self = this; ';
for (var g7i = 0; g7i < 300; g7i++) g7src += 'var w' + g7i + ' = ' + g7i + '; ';
g7src += 'var arrow = () => this.tag; return self.tag + "/" + arrow() + "/" + w299; })';
var g7 = eval(g7src);
print('G7', g7.call({ tag: 'wide' }));
