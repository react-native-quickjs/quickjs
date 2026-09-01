/* Adversarial corpus for `arguments` reification elision
   (patch 0048-arguments-elision, docs/arguments-reification-elision.md).

   The mechanism under test never builds the `arguments` object for a function
   whose only uses of `arguments` are, in the emitted bytecode:

     get_loc(arguments) ; get_field length          -> push argc
     get_loc(arguments) ; <one simple push> ; get_array_el
                                                    -> push the frame arg slot
     get_loc(arguments) ; call_method 2             -> guarded non-reifying apply

   Everything below tries to make one of those three substitutions return
   something other than what a real `arguments` object would have returned, or
   to reach a fourth use that the admission check should have rejected.

   ## Why sloppy-mode MAPPED arguments is the dangerous case

   In a sloppy-mode function with a simple parameter list the `arguments` object
   is *mapped*: `arguments[i]` and the i-th named parameter are two names for one
   storage slot. QuickJS implements that with a JSVarRef whose `pvalue` points
   straight at `sf->arg_buf[i]` (`get_var_ref`, engine/quickjs-ng/quickjs.c:20031),
   so reading `arg_buf[i]` at use time is the same read the object would have
   done. In STRICT mode, or with any non-simple parameter list, the object is
   *unmapped* and `js_build_arguments` (:18767) COPIES `argv[]` at function
   ENTRY -- so there `arguments[i]` is a snapshot, and reading the frame slot at
   use time would return a later parameter write instead. Cases 3-6 and 30-33
   are the pair that discriminates this.

   ## Two diffs, not one

   `node tests/differential/run.mjs arguments-elision` compares this file's
   output against node, which is the independent-implementation check. It is NOT
   sufficient on its own, because a handful of observables (TypeError message
   wording, `Error.prototype.stack` text) differ between QuickJS and node for
   reasons that predate this mechanism, so the corpus cannot assert on them.
   Those ARE covered by the second diff, which is stronger and specific:

       bin-base   corpus/arguments-elision.js > /tmp/a
       bin-elide  corpus/arguments-elision.js > /tmp/b
       cmp /tmp/a /tmp/b

   i.e. the same engine with the mechanism compiled out against the same engine
   with it compiled in. Any observable change at all shows up there, including
   ones node would have masked. Run both.

   ## What each guard is discriminated by

     guard                                          detected by
     ---------------------------------------------  ------------------------
     length substitution uses argc, not arg_count   cases 1, 2, 7
     element read comes from arg_buf (mapped alias) cases 3, 4, 5
     elision refused for unmapped/strict arguments  cases 30, 31, 32, 33
     elision refused when `arguments` escapes       cases 10-20
     element read out of range -> real prototype    cases 21, 22, 23
     element read with a non-index key              cases 24, 25, 26, 27
     apply guard: callee is the real builtin        cases 40, 41, 42
     apply fallback preserves object IDENTITY       case 43
     apply target not callable -> same TypeError    case 44
     apply passes argc args, extras included        cases 45, 46, 47
     apply passes CURRENT parameter values          case 48
*/

function show(x) {
  if (typeof x === 'string') return JSON.stringify(x);
  if (x === undefined) return 'undefined';
  if (x === null) return 'null';
  if (typeof x === 'object') {
    var out = [], i;
    if (Object.prototype.toString.call(x) === '[object Array]') {
      for (i = 0; i < x.length; i++) out.push(show(x[i]));
      return '[' + out.join(',') + ']';
    }
    var ks = Object.keys(x);
    for (i = 0; i < ks.length; i++) out.push(ks[i] + ':' + show(x[ks[i]]));
    return '{' + out.join(',') + '}';
  }
  return String(x);
}
function t(name, v) { print(name + ' = ' + show(v)); }

/* ---------------- length substitution ---------------- */

/* 1: length is the number of arguments PASSED, not the declared count. */
function c1(a, b, c) { return arguments.length; }
t('1a', c1());
t('1b', c1(1));
t('1c', c1(1, 2, 3, 4, 5));

/* 2: a parameter write does not change length. */
function c2(a) { a = 99; return arguments.length; }
t('2a', c2(1, 2));

/* 7: length read after the frame has been re-entered recursively. */
function c7(n) {
  if (n === 0) return [arguments.length];
  var r = c7(n - 1, 'x', 'y');
  r.push(arguments.length);
  return r;
}
t('7', c7(2));

/* ---------------- mapped element reads ---------------- */

/* 3: the classic mapping. arguments[0] must see the parameter write. */
function c3(a) { a = 99; return arguments[0]; }
t('3', c3(1));

/* 4: mapping in the other direction is NOT part of this mechanism (a write
   through arguments is a rejected use) but the read must still be right when
   the write happens via the parameter, repeatedly. */
function c4(a, b) {
  var out = [];
  out.push(arguments[0], arguments[1]);
  a = 'A2'; b = 'B2';
  out.push(arguments[0], arguments[1]);
  return out;
}
t('4', c4('A1', 'B1'));

/* 5: index is a live local, incremented -- the earleyboyer shape. */
function c5() {
  var s = '';
  for (var i = 0; i < arguments.length; i++) s += arguments[i] + '|';
  return s;
}
t('5a', c5());
t('5b', c5(1, 2, 3));
t('5c', c5('a', undefined, null, 0, false, ''));

/* 6: more arguments passed than declared, with a write to a declared one. */
function c6(a) { a = 'W'; return [arguments.length, arguments[0], arguments[1], arguments[2]]; }
t('6', c6('x', 'y', 'z'));

/* ---------------- uses that must REFUSE elision ---------------- */

/* 10: `arguments` returned. */
function c10(a) { return arguments; }
t('10a', c10(1, 2).length);
t('10b', Object.prototype.toString.call(c10(1)));
t('10c', c10(7)[0]);

/* 11: stored in a local first. */
function c11(a) { var r = arguments; return r[0] + ':' + r.length; }
t('11', c11('q', 2));

/* 12: captured by an arrow, which has no `arguments` of its own. */
function c12(a) { return function () { return 0; } && (() => arguments[0] + ':' + arguments.length); }
t('12', c12('cap', 2)());

/* 13: written through. */
function c13(a) { arguments[0] = 'through'; return a; }
t('13', c13('orig'));

/* 14: length written. */
function c14(a) { arguments.length = 1; return [arguments.length, arguments[1]]; }
t('14', c14('p', 'q'));

/* 15: `delete arguments[0]` unmaps the object. */
function c15(a) { delete arguments[0]; return [a, arguments[0], arguments.length]; }
t('15', c15('d'));

/* 16: `in`. */
function c16(a) { return [0 in arguments, 1 in arguments, 'length' in arguments]; }
t('16', c16('z'));

/* 17: for-in over arguments. */
function c17(a, b) { var ks = []; for (var k in arguments) ks.push(k); return ks; }
t('17', c17(1, 2, 3));

/* 18: spread. */
function id() { var r = []; for (var i = 0; i < arguments.length; i++) r.push(arguments[i]); return r; }
function c18() { return id.apply(null, [].concat(Array.prototype.slice.call(arguments))); }
t('18', c18(1, 2));
function c18b() { return id(...arguments); }
t('18b', c18b(3, 4));

/* 19: callee. */
function c19(a) { return arguments.callee === c19; }
t('19', c19(1));

/* 20: passed as a non-final argument, and as the sole argument. */
function two(x, y) { return show(x) + '/' + show(y); }
function c20(a) { return two(arguments, 'second'); }
t('20a', c20(1).indexOf('0:1') >= 0);
function c20b(a) { return Array.prototype.slice.call(arguments).join(','); }
t('20b', c20b(1, 2, 3));

/* ---------------- element reads that leave the fast range ---------------- */

/* 21: an index past argc resolves on the arguments object's PROTOTYPE. */
Object.prototype[5] = 'proto5';
function c21() { return arguments[5]; }
t('21a', c21(0, 1));
t('21b', c21(0, 1, 2, 3, 4, 'real5'));
delete Object.prototype[5];
t('21c', c21(0, 1));

/* 22: a negative and a huge index. */
function c22() { return [arguments[-1], arguments[4294967295], arguments[4294967296]]; }
t('22', c22('a'));

/* 23: index read on a zero-argument call. */
function c23() { return arguments[0]; }
t('23', c23());

/* 24: string keys through the element form. */
function c24() { return [arguments['length'], arguments['0'], arguments['00'], arguments['1e0']]; }
t('24', c24('s'));

/* 25: non-integral numeric index. */
function c25() { return [arguments[1.5], arguments[0.0], arguments[-0]]; }
t('25', c25('n0', 'n1'));

/* 26: `callee` and `Symbol.iterator` through the element form. */
function c26() { return [arguments['callee'] === c26, typeof arguments[Symbol.iterator]]; }
t('26', c26(1));

/* 27: an index whose ToPropertyKey has a side effect. */
function c27() {
  var seen = 0;
  var k = { valueOf: function () { seen++; return 0; } };
  var v = arguments[k];
  return [v, seen];
}
t('27', c27('sideeffect'));

/* ---------- index expressions the depth walk must model or refuse ---------- */

/* 28: earleyboyer's dominant shape -- a four-instruction index expression that
   itself contains a nested `arguments.length` site. Both must be rewritten. */
function c28() {
  var out = [arguments[arguments.length - 1]];
  for (var i = arguments.length - 2; i >= 0; i--) out.push(arguments[i]);
  return out;
}
t('28a', c28());
t('28b', c28('a'));
t('28c', c28('a', 'b', 'c'));

/* 28d: the same shape with the index falling out of range at both ends. */
function c28d() { return [arguments[arguments.length], arguments[arguments.length - 5]]; }
t('28d', c28d('x', 'y'));

/* 29: index expressions the walk must REFUSE, each for a different reason.
   Refusing means the function reifies as before, so these check that the
   refusal is correct rather than that it happens. */
var sideCount = 0;
function bump() { sideCount++; return 0; }
function c29a() { return arguments[bump()]; }           /* a call: npop fmt   */
t('29a', [c29a('called'), sideCount]);
function c29b(k) { return arguments[k ? 1 : 2]; }        /* labels in the index */
t('29b', [c29b(1, 'one', 'two'), c29b(0, 'one', 'two')]);
function c29c() { var i = 0; return [arguments[i++], arguments[i++], i]; }
t('29c', c29c('p', 'q'));
var keyObj = { k: 1 };
function c29d() { return arguments[keyObj.k]; }          /* get_field inside   */
t('29d', c29d('z0', 'z1'));
function c29e() { return arguments[[0][0]]; }            /* nested element load */
t('29e', c29e('n'));
function c29f() { var t = 0; try { t = 1; } catch (e) { t = 2; } return arguments[t]; }
t('29f', c29f('t0', 't1'));

/* ---------------- strict / unmapped: elision must not alias ---------------- */

/* 30: strict mode, simple params, parameter write. Unmapped: SNAPSHOT. */
function c30(a) { 'use strict'; a = 99; return arguments[0]; }
t('30', c30(1));

/* 31: strict length still counts what was passed. */
function c31(a, b) { 'use strict'; return arguments.length; }
t('31', c31(1));

/* 32: a DEFAULT parameter makes the list non-simple, so the object is unmapped
   even though the function is sloppy. arguments[0] must be the entry snapshot. */
function c32(a, b = 2) { a = 'W'; return [arguments.length, arguments[0], a]; }
t('32a', c32('orig'));
t('32b', c32('orig', 'given'));

/* 32c: a mapped control for 32 -- same body, simple parameter list. */
var c32c = new Function('a', 'a = "W"; return [arguments.length, arguments[0], a];');
t('32c', c32c('orig'));

/* 33: a REST parameter is also non-simple -> unmapped. */
function c33(a, ...rest) { a = 'W'; return [arguments.length, arguments[0], a, rest.length]; }
t('33', c33('o', 'p'));

/* 34: destructuring parameter -> non-simple -> unmapped. */
function c34({ x }) { x = 'W'; return [arguments.length, arguments[0].x, x]; }
t('34', c34({ x: 'orig' }));

/* 35: strict mode, more args than declared, write to a declared one. */
function c35(a) { 'use strict'; a = 'W'; return [arguments.length, arguments[0], arguments[1], a]; }
t('35', c35('o', 'p'));

/* ---------------- the apply guard ---------------- */

/* `this` is deliberately reported as a TAG, not shown: in sloppy mode
   `apply(null, ...)` substitutes the global object, and printing that would
   diff on host globals rather than on anything this corpus is about. */
function thisTag(v) {
  if (v === undefined) return '<undef>';
  if (v === null) return '<null>';
  if (typeof v === 'object' && v === globalThis) return '<global>';
  if (typeof v === 'object' && v !== null && v.name !== undefined) return '<obj:' + v.name + '>';
  if (typeof v === 'object') return '<object>';
  return '<' + typeof v + ':' + String(v) + '>';
}
function target(x, y, z) { return 'T(' + thisTag(this) + ';' + show(x) + ',' + show(y) + ',' + show(z) + ')'; }

/* 40: the plain builtin case, the raytrace shape. */
function c40() { return target.apply(null, arguments); }
t('40a', c40());
t('40b', c40(1));
t('40c', c40(1, 2, 3, 4));

/* 41: Function.prototype.apply replaced BEFORE the call site runs. */
var realApply = Function.prototype.apply;
Function.prototype.apply = function (thisArg, args) {
  return 'HOOK(' + show(args.length) + ',' + show(args[0]) + ',' + (Object.prototype.toString.call(args)) + ')';
};
function c41() { return target.apply(null, arguments); }
t('41', c41('h1', 'h2'));
Function.prototype.apply = realApply;
t('41b', c41('h1', 'h2'));

/* 42: an own `apply` on the target shadowing the builtin. */
var shadow = function () { return 'never'; };
shadow.apply = function (thisArg, args) { return 'SHADOW(' + args.length + ',' + args[0] + ')'; };
function c42() { return shadow.apply(null, arguments); }
t('42', c42('s1', 's2'));

/* 43: IDENTITY. When the guard misses, the object that escapes must be the
   SAME object at both use sites in one call -- one `arguments` per frame. */
var seenA = null, seenB = null;
var recorder = function () { return 'rec'; };
recorder.apply = function (thisArg, args) { if (seenA === null) seenA = args; else seenB = args; return 'R'; };
function c43() {
  recorder.apply(null, arguments);
  recorder.apply(null, arguments);
  return seenA === seenB;
}
t('43', c43('i1'));

/* 44: apply target not callable. Only the error TYPE is compared here, because
   QuickJS and node word these messages differently and always have -- that is
   not what this corpus is about. The message text IS covered, more strictly,
   by the qjs-before/qjs-after byte diff described in the header. */
function c44(t) {
  try { return t.apply(null, arguments); }
  catch (e) { return e.constructor.name + '/' + (/not a function|reading 'apply'|of undefined/.test(e.message) ? 'expected-message' : 'UNEXPECTED: ' + e.message); }
}
t('44a', c44(1));
t('44b', c44(undefined));
t('44c', c44({}));

/* 45: extras beyond the declared count are all forwarded. */
function c45(a) { return target.apply('R', arguments); }
t('45', c45('p', 'q', 'r', 's'));

/* 46: zero arguments. */
function c46() { return target.apply('R', arguments); }
t('46', c46());

/* 47: the target reads its own arguments.length. */
function counter() { return arguments.length + ':' + Array.prototype.join.call(arguments, '-'); }
function c47() { return counter.apply(null, arguments); }
t('47', c47('a', 'b', 'c'));

/* 48: apply must forward the CURRENT parameter values (mapped). */
function c48(a, b) { a = 'A2'; return target.apply(null, arguments); }
t('48', c48('A1', 'B1', 'C1'));

/* 49: the target writes its own parameters -- must not corrupt our frame. */
function writer(x, y) { x = 'X'; y = 'Y'; return x + y; }
function c49(a, b) { var r = writer.apply(null, arguments); return [r, a, b, arguments[0], arguments[1]]; }
t('49', c49('a0', 'b0'));

/* 50: mixed uses in one function -- length, element, and apply. */
function c50(a) { return [arguments.length, arguments[0], target.apply(null, arguments)]; }
t('50', c50('m', 'n'));

/* 51: apply inside a method, with `this`. */
var obj = { name: 'obj', run: function () { return target.apply(this, arguments); } };
t('51', obj.run(1, 2));

/* 52: apply where the target is a bound function. */
var bound = target.bind('BOUND', 'first');
function c52() { return bound.apply(null, arguments); }
t('52', c52('second'));

/* 53: apply where the target is a native function. */
function c53() { return Math.max.apply(null, arguments); }
t('53', c53(3, 9, 4));

/* 54: apply where the target throws -- exception propagates, message intact. */
function thrower() { throw new TypeError('from thrower ' + arguments.length); }
function c54() { try { return thrower.apply(null, arguments); } catch (e) { return e.message; } }
t('54', c54(1, 2));

/* 55: Reflect.apply and .call are separate paths and must be untouched. */
function c55() { return [Reflect.apply(target, 'RA', arguments), target.call(null, arguments.length)]; }
t('55', c55(1, 2));

/* 56: apply through a getter-provided function. */
var holder = {};
Object.defineProperty(holder, 'fn', { get: function () { return target; } });
function c56() { return holder.fn.apply('G', arguments); }
t('56', c56('g'));

/* 57: the raytrace shape verbatim -- Prototype.js style Class.create. */
function makeClass(init) {
  var K = function () { this.initialize.apply(this, arguments); };
  K.prototype.initialize = init;
  return K;
}
var Pt = makeClass(function (x, y) { this.x = x; this.y = y; });
var pt = new Pt(3, 4);
t('57', [pt.x, pt.y]);

/* 58: constructor path -- `new` on a function that applies its arguments. */
function c58base(x) { this.v = x; }
function C58() { c58base.apply(this, arguments); }
t('58', new C58('ctor').v);

/* 59: generator with arguments read across a yield. */
function* gen(a) { yield arguments.length; a = 'W'; yield arguments[0]; yield arguments[1]; }
var g = gen('g0', 'g1');
t('59', [g.next().value, g.next().value, g.next().value]);

/* 60: a parameter literally named `arguments`. */
function c60(arguments) { return [arguments, typeof arguments]; }
t('60', c60('shadowed'));

/* 61: direct eval can see `arguments`. */
function c61(a) { return eval('arguments[0] + ":" + arguments.length'); }
t('61', c61('e', 2));

/* 62: `with` in scope. */
function c62(a) { var o = { arguments: 'from-with' }; with (o) { return arguments; } }
t('62', c62('x'));

/* 63: apply on an arguments object that came from ANOTHER frame. */
function c63outer() { return c63inner(arguments); }
function c63inner(outerArgs) { return target.apply(null, outerArgs); }
t('63', c63outer('o1', 'o2'));

/* 64: many arguments -- crosses any small-count assumption. */
function c64() { return arguments.length + ':' + arguments[0] + ':' + arguments[299]; }
var big = [];
for (var i = 0; i < 300; i++) big.push(i);
t('64', c64.apply(null, big));
function c65() { return counter.apply(null, arguments); }
t('65', c65.apply(null, big).length);

/* 66: apply where thisArg has a side effect evaluated before arguments. */
var order = [];
function sideThis() { order.push('this'); return 'ST'; }
function c66() { order.push('enter'); return target.apply(sideThis(), arguments) + '|' + order.join(','); }
t('66', c66('z'));

/* 67: tail position. */
function c67() { return target.apply(null, arguments); }
t('67', c67('t'));

/* 68: arguments used only in a dead branch that never executes. */
function c68(a) { if (a) return arguments.length; return 'no'; }
t('68a', c68(0));
t('68b', c68(1, 2));

/* ======================================================================
   CASES 80+ were added after a mutation test showed cases 1-70 killed only
   10 of 31 deliberately broken builds. Each block below exists to
   discriminate one specific guard that the first corpus did not reach; the
   comment says which mutant it kills. See
   docs/arguments-reification-elision.md for the full matrix.
   ====================================================================== */

/* 80: the apply identity guard, properly. Cases 41-43 replaced `apply` with a
   plain JS function, which fails the class_id test on its own -- so breaking any
   ONE of the four identity conditions still left another catching it, and all
   four mutants survived. These replace it with C functions that match some of
   the conditions and not others.

   kills: apply-identity-magic     -- Reflect.apply IS js_function_apply, magic 2
   kills: apply-identity-fn        -- Function.prototype.call is a C function
                                      with the same cproto, different entry point
   kills: apply-identity-cproto    -- Math.max is a C function with a different
                                      cproto whose union overlaps differently
   kills: PAIR-identity-fn+magic, PAIR-identity-classid+cproto */
function idTarget() { return 'ID(' + arguments.length + ':' + Array.prototype.join.call(arguments, '-') + ')'; }

var t80a = function () { return 'raw-a'; };
t80a.apply = Reflect.apply;             /* same C fn, magic 2 */
function c80a() { return t80a.apply(idTarget, null, arguments); }
t('80a', c80a(1, 2));

var t80b = function () { return 'raw-b'; };
t80b.apply = Function.prototype.call;   /* C fn, generic, not apply */
function c80b() { return t80b.apply(idTarget, arguments); }
t('80b', c80b(1, 2));

/* 80c is the one that matters for the entry-point check. Math.min is registered
   JS_CFUNC_MAGIC_DEF("min", 2, js_math_min_max, 0) -- a C function with cproto
   generic_magic AND magic 0, so it satisfies every identity condition EXCEPT
   `c_function.generic_magic == js_function_apply`. Math.max is the same entry
   point with magic 1, which is why it does not discriminate that check. */
var t80c = { apply: Math.min };
function c80c() { return t80c.apply(1, arguments); }
t('80c', c80c(5, 9));
var t80c2 = { apply: Math.max };
function c80c2() { return t80c2.apply(1, arguments); }
t('80c2', c80c2(5, 9));
var t80c3 = { apply: Array.prototype.pop };
function c80c3() { try { return t80c3.apply([1, 2], arguments); } catch (e) { return e.constructor.name; } }
t('80c3', c80c3(5, 9));

var t80d = function () { return 'raw-d'; };
t80d.apply = Function.prototype.apply.bind(idTarget);  /* bound builtin */
function c80d() { return t80d.apply(null, arguments); }
t('80d', c80d(3, 4));

/* 81: apply target that IS callable-looking but is not callable, reached with
   the REAL builtin apply. Case 44 could never get here: `(1).apply` and
   `({}).apply` are undefined, so the property load threw before the opcode ran.
   An object whose prototype is Function.prototype has a real `.apply` and is
   still not callable, which is the only way to reach the check.

   kills: apply-no-check-function */
var notCallable = Object.create(Function.prototype);
function c81() {
  try { return notCallable.apply(null, arguments); }
  catch (e) { return e.constructor.name; }
}
t('81', c81(1, 2));

/* 82: a MATERIALIZED frame with more arguments than declared, so the argument
   list is split between arg_buf and the caller's argv. Reached only through a
   native shim that requests JS_CALL_FLAG_COPY_ARGV -- Array.prototype.forEach
   passes three arguments to a callback that declares one. Nothing in cases 1-70
   was ever called that way, so the split was never exercised at all.

   kills: apply-ignores-split, el-reads-argv-always, el-reads-argbuf-always */
var out82 = [];
[10, 20].forEach(function (a) {
  a = 'W';                     /* a parameter write: arg_buf[0] now differs
                                  from argv[0], which is the whole point */
  out82.push([arguments.length, arguments[0], arguments[1],
              idTarget.apply(null, arguments)]);
});
t('82', out82);

var out82b = [];
[7].forEach(function (a, b, c) { out82b.push([arguments.length, arguments[0], arguments[2].length]); });
t('82b', out82b);

/* 82c: the same split shape through a different shim, and with the elided
   function also reading a high index. */
t('82c', [1, 2, 3].map(function (a) { a = a * 10; return [arguments[0], arguments[1], arguments.length]; }));
t('82d', ['x', 'y'].filter(function (a) { a = 'Z'; return arguments[0] === 'x'; }));
t('82e', [1, 2].reduce(function (acc, v) { v = 99; return acc + arguments[1] + ':' + arguments.length + ';'; }, 'R:'));

/* 83: an ASSIGNMENT to `arguments`, which is the only thing that reaches the
   "the opcode naming this slot is not a read" branch. Cases 11 and 13 look like
   they should, but `var r = arguments` and `arguments[0] = v` are both refused
   one step earlier, by the consumer classification.

   kills: accept-non-get-loc */
function c83a() { var n = arguments.length; arguments = 'reassigned'; return [n, arguments]; }
t('83a', c83a(1, 2));
function c83b(a) { arguments = { 0: 'fake', length: 1 }; return [arguments[0], arguments.length, a]; }
t('83b', c83b('real'));
function c83c() { if (arguments.length === 0) arguments = 'empty'; return String(arguments); }
t('83c', c83c());
t('83d', c83c(1));

/* 84: `.apply` calls whose argument count is not 2. With the argc test removed,
   `g.apply(arguments)` becomes a 3-pop opcode over a 2-value stack.

   kills: apply-argc-unchecked */
/* `g.apply(arguments)` is a ONE-argument method call whose only argument is
   `arguments`, so it is in the last position and the argc test is the only thing
   that rejects it. Its stack effect happens to match apply_arguments' (3 pop,
   1 push), so compute_stack_size does NOT catch the mutant -- only the value
   does: the spec answer is `g.call(argumentsObject)` with no arguments, whereas
   the mutant forwards our whole argument list. Printing `typeof` instead of the
   value is what let this mutant survive the first attempt. */
function c84a() { return idTarget.apply(arguments); }
t('84a', c84a(1, 2));
function c84b() { var o = { m: function () { return 'm' + arguments.length; } }; return o.m(1, 2, arguments); }
t('84b', c84b('q'));
function c84c() { return idTarget.apply(null, arguments, 'extra'); }
t('84c', c84c(1, 2));

/* 85: index expressions that make the depth walk's bail-outs load-bearing.

   kills: walk-ignores-depth -- a get_array_el at depth 1 (a nested load whose
          own receiver is not `arguments`) must NOT be mistaken for ours
   kills: walk-allows-npop   -- a call inside the index whose operand pop count
          the walk cannot model
   kills: walk-allows-labels -- a branch inside the index */
var tbl85 = [[0, 1], [1, 0]];
function c85a(k) { return arguments[tbl85[k][1]]; }
t('85a', [c85a(0, 'A', 'B'), c85a(1, 'A', 'B')]);
function c85b() { return [arguments[0], tbl85[0][1], arguments[1]]; }
t('85b', c85b('p', 'q'));
function id85(x) { return x; }
function c85c() { return arguments[id85(1)]; }
t('85c', c85c('m', 'n'));
function c85d(f) { return arguments[f ? 1 : 2] + '/' + arguments[1]; }
t('85d', [c85d(1, 'one', 'two'), c85d(0, 'one', 'two')]);
function c85e() { return arguments[arguments.length - 1] + '/' + arguments[[1][0]]; }
t('85e', c85e('u', 'v'));

/* 86: `arguments` reachable only through direct eval, or through a `with` whose
   object might supply it, in functions that contain NO `arguments` read of their
   own. The elision would drop the prologue and the eval would then see an
   uninitialized local.

   kills: no-eval-check, no-shape-check (jointly with the captured check --
   see the PAIR entries, since QuickJS also marks the slot captured for a
   direct eval) */
function c86a() { return eval('arguments.length'); }
t('86a', c86a(1, 2, 3));
function c86b(a) { var r = eval('arguments'); return [r.length, r[0], typeof r]; }
t('86b', c86b('e0', 'e1'));
function c86c() { var o = {}; with (o) { return eval('arguments[0]'); } }
t('86c', c86c('w0'));
function c86d() { var o = { arguments: 'shadow' }; with (o) { return arguments; } }
t('86d', String(c86d('x')));
function c86e() { return (function () { return eval('arguments.length'); })(1, 2); }
t('86e', c86e());

/* 87: parameter-expression scopes, where `arguments` gets a binding in the
   argument scope as well as the body scope.

   kills: no-arguments-arg-check (attempted -- see the docs page for the
   measured outcome) */
function c87a(a = arguments.length) { return [a, arguments.length]; }
t('87a', [c87a(), c87a(5), c87a(5, 6)]);
function c87b(a = 1, b = arguments[0]) { return [a, b, arguments.length]; }
t('87b', [c87b(), c87b('p'), c87b('p', 'q')]);
function c87c({ x = arguments.length } = {}) { return [x, arguments.length]; }
t('87c', [c87c(), c87c({}), c87c({ x: 9 })]);

/* 88: the memo. Two DIFFERENT elided sites in one frame must hand the same
   object to a non-builtin apply, and a slow element read must hand back that
   same object too. Case 43 used one site twice; this uses two distinct sites,
   and mixes an element site in.

   kills: memo-not-memoized (already killed by 43, kept as the stronger form) */
var seen88 = [];
var rec88 = function () { return 'r'; };
rec88.apply = function (thisArg, args) { seen88.push(args); return 'R'; };
Object.defineProperty(Object.prototype, 'probe88', {
  configurable: true,
  get: function () { seen88.push(this); return 'P'; }
});
function c88() {
  rec88.apply(null, arguments);
  var p = arguments['probe88'];
  rec88.apply(null, arguments);
  return [p, seen88.length, seen88[0] === seen88[1], seen88[1] === seen88[2]];
}
t('88', c88('i1'));
delete Object.prototype.probe88;

/* 89: `arguments.length` where the declared parameter count differs from the
   passed count in both directions, inside a frame that materialized. sf->
   arg_count is raised to b->arg_count on that path, so a length substitution
   reading it instead of argc is wrong only here.

   kills: len-uses-sf-arg-count (already killed by case 1, kept because this is
   the materialized variant, which is a different code path) */
var out89 = [];
[0].forEach(function (a, b, c, d) { out89.push(arguments.length); });
t('89a', out89);
function c89(a, b, c, d, e) { return arguments.length; }
t('89b', [c89(), c89(1), c89(1, 2, 3, 4, 5, 6, 7)]);

/* 90: argument capture. With the capture loop removed, a mapped object built on
   a slow path gets DETACHED var_refs holding copies, so it silently stops
   aliasing the parameters.

   kills: no-capture-args */
var esc90 = null;
var grab90 = function () { return 'g'; };
grab90.apply = function (thisArg, args) { esc90 = args; return 'G'; };
function c90(a, b) {
  grab90.apply(null, arguments);
  a = 'A2';
  b = 'B2';
  return [esc90[0], esc90[1], esc90.length, a, b];
}
t('90', c90('A1', 'B1'));

/* 91: argv_safe. If the elision let a COPY_ARGV caller skip materializing, a
   mapped object built on the slow path would alias the CALLER's stack, and a
   write through it would reach into the caller's frame.

   kills: argv-safe-not-refused (attempted -- see the docs page) */
var esc91 = [];
var grab91 = function () { return 'g'; };
grab91.apply = function (thisArg, args) { esc91.push(args); args[0] = 'CLOBBER'; return 'G'; };
function c91(a) { grab91.apply(null, arguments); return [a, arguments.length]; }
t('91a', [1, 2].map(c91));
t('91b', c91('direct'));
t('91c', esc91.length);

/* 70: backtrace shape. The non-reifying apply does NOT push a native
   `Function.prototype.apply` frame, so `e.stack` loses the "at apply (native)"
   line that the reifying path produced. That is a real, intended observable
   change -- it makes us match node, which has no such frame -- and it is the
   ONE line the base/patched byte diff is expected to differ on. What must not
   change is that both JS frames are present and in order. */
function c70a() { throw new Error('boom'); }
function c70b() { return c70a.apply(this, arguments); }
function c70() {
  try { c70b(1, 2); } catch (e) {
    var s = String(e.stack);
    return [s.indexOf('c70a') >= 0, s.indexOf('c70b') >= 0,
            s.indexOf('c70a') < s.indexOf('c70b')];
  }
}
t('70', c70());

/* 69: exception thrown between the prologue and the use. */
function c69(a) {
  try { null.x; } catch (e) { /* ignore */ }
  return arguments.length;
}
t('69', c69(1, 2, 3));
