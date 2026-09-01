/* Adversarial corpus for the `var a = arguments` ALIAS admission rule
   (round 5 item 2, docs/round5-earleyboyer-items.md; extends patch 0048 and
   tests/differential/corpus/arguments-elision.js).

   Patch 0048 elides the `arguments` object only when every use of the compiler's
   `arguments` local is one of three adjacent-consumer shapes. It refuses

       function sc_list() { var res = null;
                            var a = arguments;        // get_loc(A) put_loc(B)
                            for (var i = a.length-1; i >= 0; i--)
                                res = new sc_Pair(a[i], res);
                            return res; }

   because the object is copied to a local first. The alias rule admits exactly
   that: one store, into a non-captured local, dominating every read, where each
   read is itself one of 0048's approved consumers.

   ## The three conditions, and the case that discriminates each

     condition                                       cases
     ----------------------------------------------  ----------------------
     the store DOMINATES every read of the alias     12, 13, 14, 15, 16, 17
     the alias is written EXACTLY ONCE               20, 21, 22
     every READ of the alias is an approved consumer 30-39, 44, 45, 46
     the alias is not captured by a closure          40, 41, 42
     mapped-vs-unmapped is decided as 0048 decides   50-55
     the alias and a direct `arguments` use agree    60, 61, 62

   Case 12 is the reason the dominance condition exists rather than a cheaper
   positional test: the store precedes the read in POSITION and not in
   EXECUTION, so a positional test would rewrite `a.length` on a path where `a`
   is undefined and return argc instead of throwing.

   TWO OF THESE CASES WERE ADDED BECAUSE A MUTANT SURVIVED, and they are the
   most valuable lines in the file. Case 16 kills `alias-no-dominance-branches`,
   which case 12 does not, because `if (c) {} else { store }` emits an OP_label
   before the store and a second guard refuses it. Case 44 kills
   `alias-accepts-any-use`, which nothing in cases 1-42 does, because it is the
   only shape where a non-`get_loc` opcode names the alias in a function the
   entry-block rule still admits. See docs/round5-earleyboyer-items.md 2.5.

   ## Two diffs, not one

   `node tests/differential/run.mjs arguments-alias-elision` is the
   independent-implementation check. The stronger one is the same engine with the
   rule switched off, which catches observables node masks (TypeError wording,
   stack text):

       QJS_ARGS_ALIAS=0 bin corpus/arguments-alias-elision.js > /tmp/a
                        bin corpus/arguments-alias-elision.js > /tmp/b
       cmp /tmp/a /tmp/b

   Run both.
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
function tt(name, f) {
  try { t(name, f()); }
  catch (e) { print(name + ' = threw ' + (e && e.constructor ? e.constructor.name : e)); }
}

/* ---------------- 1-9: the shape the rule exists for ---------------- */

/* 1: earleyboyer's sc_list, verbatim in shape. length site + element site,
   the element index being a live local counting down. */
function c1() {
  var res = null;
  var a = arguments;
  for (var i = a.length - 1; i >= 0; i--) res = [a[i], res];
  return res;
}
t('1a', c1());
t('1b', c1(1));
t('1c', c1(1, 2, 3));
t('1d', c1(undefined, null, 0, false, ''));

/* 2: alias with a length site only. */
function c2(x, y) { var a = arguments; return a.length; }
t('2a', c2());
t('2b', c2(1));
t('2c', c2(1, 2, 3, 4));

/* 3: MAPPED aliasing. Sloppy mode + simple parameter list: a[0] must observe
   the parameter write, because the object would have. */
function c3(x) { var a = arguments; x = 99; return a[0]; }
t('3', c3(1));

/* 4: mapping is live in both directions of time -- read, write, read. */
function c4(x, y) {
  var a = arguments, out = [];
  out.push(a[0], a[1]);
  x = 'X2'; y = 'Y2';
  out.push(a[0], a[1]);
  return out;
}
t('4', c4('X1', 'Y1'));

/* 5: the `a[a.length - 1]` shape -- an element site whose index expression
   contains another site on the same alias. */
function c5() { var a = arguments; return a[a.length - 1]; }
tt('5a', function () { return c5(); });
t('5b', c5(7));
t('5c', c5(7, 8, 9));

/* 6: alias to `.apply`. */
function sum6() { var s = 0; for (var i = 0; i < arguments.length; i++) s += arguments[i]; return s; }
function c6() { var a = arguments; return sum6.apply(null, a); }
t('6a', c6());
t('6b', c6(1, 2, 3));

/* 7: recursion -- each frame's alias must be its own frame. */
function c7(n) {
  var a = arguments;
  if (n === 0) return [a.length];
  var r = c7(n - 1, 'x', 'y');
  r.push(a.length);
  return r;
}
t('7', c7(2));

/* 8: alias in a function that also declares parameters, more args than
   parameters, and reads past the declared count. */
function c8(x) { var a = arguments; return [x, a[0], a[1], a[2], a.length]; }
t('8', c8(1, 2, 3));

/* 9: alias read out of range, and at a hole-ish index. */
function c9() { var a = arguments; return [a[0], a[5], a[-1], a.length]; }
t('9a', c9());
t('9b', c9('p'));

/* ---------------- 12-15: DOMINANCE ---------------- */

/* 12: THE case. The store precedes the read positionally and is skipped at
   run time on one path, so `a` is undefined there and `a.length` must throw. */
function c12(cond) {
  if (cond) { } else { var a = arguments; }
  return a.length;
}
tt('12a', function () { return c12(1); });
t('12b', c12(0));

/* 16: the store is inside a THEN branch with no else. This is the case that
   discriminates the branch half of the dominance rule from the terminator half:
   for `if (c) {} else { store }` the compiler emits an OP_label before the
   store, so the terminator deny-list refuses it even with the branch check
   removed. Here nothing but the `if_false` precedes the store, so only the
   branch check refuses it -- and with that check removed `a.length` returns
   argc on the path where `a` is undefined instead of throwing. Case 12 alone
   does NOT discriminate this; it was added after the mutation matrix reported
   `alias-no-dominance-branches` as SURVIVED. */
function c16(cond) {
  if (cond) { var a = arguments; }
  return a.length;
}
t('16a', c16(1, 'p'));
tt('16b', function () { return c16(0); });

/* 17: the same, with the read in the same branch as the store (which IS
   dominated) plus a second read outside it (which is not). */
function c17(cond) {
  var n = -1;
  if (cond) { var a = arguments; n = a.length; }
  return [n, typeof a];
}
t('17a', c17(1, 'q', 'r'));
t('17b', c17(0));

/* 13: the store is inside a loop body that may run zero times. */
function c13(n) {
  for (var i = 0; i < n; i++) { var a = arguments; }
  return a === undefined ? 'undefined' : a.length;
}
t('13a', c13(0));
t('13b', c13(1, 'z'));

/* 14: the store is inside a try that a throw jumps out of before it runs. */
function c14(bad) {
  try { if (bad) throw new Error('x'); var a = arguments; } catch (e) { }
  return a === undefined ? 'undefined' : a.length;
}
t('14a', c14(0));
t('14b', c14(1));

/* 15: the store is guarded by a && short-circuit. */
function c15(cond) {
  var a;
  cond && (a = arguments);
  return a === undefined ? 'undefined' : a.length;
}
t('15a', c15(0));
t('15b', c15(1, 2));

/* ---------------- 20-22: SINGLE ASSIGNMENT ---------------- */

/* 20: the alias is overwritten with something else, then read. */
function c20() { var a = arguments; a = [9, 9, 9]; return [a.length, a[0]]; }
t('20', c20(1));

/* 21: the alias is overwritten with the SAME kind of thing from another frame. */
function inner21() { return arguments; }
function c21() { var a = arguments; a = inner21(1, 2, 3); return a.length; }
t('21', c21('only'));

/* 22: the alias is incremented -- a read-modify-write of the slot. */
function c22() { var a = arguments; a = a.length; return a; }
t('22', c22(1, 2));

/* ---------------- 30-39: the READS must be approved consumers ---------- */

/* 30: the alias escapes as a call argument. */
function take30(o) { return [Object.prototype.toString.call(o), o.length, o[0]]; }
function c30() { var a = arguments; return take30(a); }
t('30', c30('q', 'r'));

/* 31: the alias is returned. */
function c31() { var a = arguments; return a; }
t('31a', Object.prototype.toString.call(c31(1)));
t('31b', c31(1, 2).length);
t('31c', c31(1, 2)[1]);

/* 32: `a.callee`, which only a real object has. */
function c32() { var a = arguments; return typeof a.callee; }
t('32', c32());

/* 33: a non-`length` property read. */
function c33() { var a = arguments; return [a.foo, a.constructor === Object]; }
t('33', c33(1));

/* 34: the alias is iterated with for-of via its Symbol.iterator. */
function c34() { var a = arguments, out = []; for (var i = 0; i < a.length; i++) out.push(a[i]); var b = arguments; for (var k in b) out.push(k); return out; }
t('34', c34('m', 'n'));

/* 35: `a` is written through -- arguments[i] = v, which 0048 refuses even
   without an alias. */
function c35(x) { var a = arguments; a[0] = 'W'; return [x, a[0]]; }
t('35', c35('orig'));

/* 36: `delete` on the alias. */
function c36(x) { var a = arguments; delete a[0]; return [x, a[0], a.length]; }
t('36', c36('d'));

/* 37: the alias is compared for identity against `arguments`. */
function c37() { var a = arguments; return a === arguments; }
t('37', c37(1));

/* 38: a chained alias, `var b = a`. */
function c38() { var a = arguments; var b = a; return b.length; }
t('38', c38(1, 2, 3));

/* 39: an element read whose index is a string, a float, and a negative zero. */
function c39() { var a = arguments; return [a['0'], a[0.0], a[-0], a['00'], a[1.5]]; }
t('39', c39('z0', 'z1'));

/* 44: a `let` alias whose read needs a TDZ check. This is the case that
   discriminates the "every non-read use of the alias refuses the function"
   guard. `let a = arguments` emits set_loc_uninitialized(a) BEFORE the store,
   and the read inside the `let i` loop is a get_loc_check, not a get_loc. A
   build that merely skips the non-read use instead of refusing leaves the local
   permanently uninitialized (the store is elided) and the read throws
   `ReferenceError: a is not initialized`. Added after the mutation matrix
   reported `alias-accepts-any-use` as SURVIVED on cases 1-42. */
function c44() {
  let a = arguments;
  let s = 0;
  for (let i = 0; i < 2; i++) { s += a.length; }
  return s;
}
t('44', c44(1, 2));

/* 45: a `const` alias. */
function c45() { const a = arguments; return [a.length, a[0]]; }
t('45', c45('k'));

/* 46: a `let` alias read in a nested block, plus a direct `arguments` use. */
function c46(x) {
  let a = arguments;
  let out = [];
  { out.push(a[0]); }
  out.push(arguments.length, a.length);
  return out;
}
t('46', c46('n0', 'n1'));

/* ---------------- 40-42: CAPTURE ---------------- */

/* 40: a nested function reads the alias -- the object outlives the frame. */
function c40() { var a = arguments; return function () { return [a.length, a[0]]; }; }
t('40', c40('cap', 'tured')());

/* 41: an arrow captures the alias (an arrow has no `arguments` of its own). */
function c41() { var a = arguments; var f = function () { return a[1]; }; return f(); }
t('41', c41('u', 'v'));

/* 42: the alias is captured and the frame's parameter is then written -- the
   captured object must still be mapped. */
function c42(x) { var a = arguments; var f = function () { return a[0]; }; x = 'later'; return f(); }
t('42', c42('first'));

/* ---------------- 50-55: MAPPED vs UNMAPPED ---------------- */

/* 50: strict mode -- the object is a snapshot, so a later parameter write must
   NOT be visible through the alias. */
function c50(x) { 'use strict'; var a = arguments; x = 99; return a[0]; }
t('50', c50(1));

/* 51: strict mode length. */
function c51(x) { 'use strict'; var a = arguments; return a.length; }
t('51', c51(1, 2, 3));

/* 52: non-simple parameter list (default value) -- unmapped even in sloppy. */
function c52(x, y) { var a = arguments; x = 99; return [a[0], a.length]; }
t('52', c52(1, 2));

/* 53: rest parameter -- `arguments` is unmapped. */
function c53(x) { var a = arguments; x = 99; return [a[0], a.length]; }
t('53', c53(1, 2, 3));

/* 54: destructuring parameter. */
function c54(o) { var a = arguments; return [a.length, a[0].k]; }
t('54', c54({ k: 'kk' }));

/* 55: the alias inside a generator. */
function c55(x) {
  var a = arguments;
  return [a.length, a[0], x];
}
function* g55(x) {
  var a = arguments;
  yield a.length;
  yield a[0];
  x = 'written';
  yield a[0];
}
t('55a', c55('g'));
t('55b', Array.prototype.slice.call((function () { var out = [], it = g55('gen', 2), r; while (!(r = it.next()).done) out.push(r.value); return out; })()));

/* ---------------- 60-62: alias mixed with direct uses ---------------- */

/* 60: both `a.length` and `arguments.length` in the same function. */
function c60() { var a = arguments; return [a.length, arguments.length, a[0], arguments[0]]; }
t('60', c60('s', 't'));

/* 61: an alias in a function whose `arguments` also escapes directly. */
function c61() { var a = arguments; var b = arguments; return [a.length, Object.prototype.toString.call(b)]; }
t('61', c61(1));

/* 62: two aliases of `arguments`. */
function c62() { var a = arguments; var b = arguments; return [a.length, b[0]]; }
t('62', c62('x', 'y'));

/* 63: an alias in a function that also uses `with` (the binding could be
   exposed by name). */
function c63(o) {
  var a = arguments;
  var out;
  with (o) { out = k; }
  return [out, a.length];
}
t('63', c63({ k: 'wk' }));

/* 64: an alias in a function containing a direct eval, which can name it. */
function c64() {
  var a = arguments;
  return [eval('a.length'), a[0]];
}
t('64', c64('e0', 'e1'));

/* 65: the alias is named `arguments` itself. */
function c65(x) { var args = arguments; var arguments2 = args.length; return arguments2; }
t('65', c65(1, 2));

/* 66: an alias whose reads happen in a nested block scope. */
function c66() {
  var a = arguments;
  var out = [];
  { out.push(a.length); }
  if (a.length) { out.push(a[0]); }
  while (out.length < 3) { out.push(a[a.length - 1]); }
  return out;
}
t('66', c66('L', 'M'));

/* 67: alias in a function called with a `this` and as a constructor. */
function C67() { var a = arguments; this.n = a.length; this.first = a[0]; }
t('67a', new C67('c', 'd').n);
t('67b', new C67('c', 'd').first);
t('67c', (function () { var o = {}; C67.call(o, 'e'); return o.n + ':' + o.first; })());

/* 68: alias in a function invoked through Function.prototype.apply with a
   large-ish argument list. */
function c68() { var a = arguments; var s = 0; for (var i = 0; i < a.length; i++) s += a[i]; return s; }
t('68', c68.apply(null, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]));

/* 69: alias read when zero arguments were passed and the parameter list is
   non-empty -- the frame's undeclared slots must read as undefined. */
function c69(x, y, z) { var a = arguments; return [a.length, a[0], a[2], x, z]; }
t('69', c69());
