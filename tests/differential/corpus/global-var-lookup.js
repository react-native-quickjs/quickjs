/* Differential corpus for global variable lookup — the `global_var_obj`
   empty-shape early-out (patch "global-var empty-shape early-out", R1-2 of
   docs/the-three-rounds.md).

   The mechanism under test skips the `find_own_property` probe of
   `ctx->global_var_obj` inside `JS_GetGlobalVar` / `JS_SetGlobalVar` when that
   object's shape has `prop_count == 0`.  `global_var_obj` holds *only* global
   lexical bindings — `let`, `const` and `class` at script top level.  Every
   case below therefore tries to make a global lexical binding matter:

     - a lexical binding that SHADOWS a same-named property of globalThis
       (cases 2, 3, 9) — if the probe is skipped the globalThis value leaks out;
     - the temporal dead zone (cases 4, 10) — if the probe is skipped the read
       finds nothing in global_var_obj and either throws the wrong error or
       returns the globalThis value;
     - assignment to a `const` (case 5) and to a shadowed `let` (case 3), which
       is the JS_SetGlobalVar half of the same edit;
     - the surface the plan asked for: `delete globalThis.x` between two reads,
       an accessor installed on globalThis after a site is warm, a Proxy on
       globalThis's prototype, `with`, and direct `eval` introducing a binding.

   Note on the two runs.  `qjs-bench` evaluates this file as a *script*, so
   top-level `let`/`const` create real global lexical bindings and the engine
   reaches them through `global_var_obj`.  node evaluates it as an ES module, so
   the same declarations are module-scoped.  The two engines reach the bindings
   by different internal routes; the observable results are identical, which is
   exactly what a differential test is for.  It also means node cannot see a
   *missing* global lexical binding as anything other than a missing module
   binding — the error text is normalised below for that reason.

   Errors are reported as `constructor.name` only, never `e.message`: message
   text is not specified and differs between engines. */

function show(f) {
  try { print(f()); } catch (e) { print('threw ' + e.constructor.name); }
}

/* ---- CASE 1: an ordinary globalThis property, read many times so a warm
   site exists before anything below perturbs it. */
globalThis.g1 = 'g1-original';
function readG1() { return g1; }
var s = '';
for (var i = 0; i < 200; i++) s = readG1();
print('1 ' + s);

/* ---- CASE 2: a global lexical binding SHADOWING a globalThis property of the
   same name.  The lexical wins.  A build that skips the global_var_obj probe
   prints 'shadowed-by-globalThis'. */
globalThis.g2 = 'shadowed-by-globalThis';
let g2 = 'lexical-g2';
function readG2() { return g2; }
for (var i2 = 0; i2 < 200; i2++) s = readG2();
print('2 ' + s);
print('2b ' + globalThis.g2);

/* ---- CASE 3: writing through the shadowed name must hit the lexical binding,
   not the globalThis property (this is the JS_SetGlobalVar half). */
function writeG2(v) { g2 = v; }
writeG2('lexical-g2-updated');
print('3 ' + readG2() + ' | ' + globalThis.g2);

/* ---- CASE 4: temporal dead zone.  `g4` is declared `let` further down, and a
   globalThis property of the same name exists.  Reading it before the
   declaration must throw, not produce the globalThis value. */
globalThis.g4 = 'g4-from-globalThis';
function readG4() { return g4; }
show(readG4);
let g4 = 'lexical-g4';
print('4b ' + readG4());

/* ---- CASE 5: assignment to a global `const` must throw TypeError. */
const g5 = 'const-g5';
function writeG5() { g5 = 'nope'; return 'no throw'; }
show(writeG5);
print('5b ' + g5);

/* ---- CASE 6: `delete globalThis.x` between two reads of x. */
globalThis.g6 = 'g6-present';
function readG6() { return g6; }
print('6 ' + readG6());
delete globalThis.g6;
show(readG6);

/* ---- CASE 7: an accessor installed on globalThis AFTER the site is warm. */
globalThis.g7 = 'g7-data';
function readG7() { return g7; }
for (var i7 = 0; i7 < 200; i7++) s = readG7();
print('7 ' + s);
delete globalThis.g7;
var g7calls = 0;
Object.defineProperty(globalThis, 'g7', {
  configurable: true,
  get: function () { g7calls++; return 'g7-accessor-' + g7calls; }
});
print('7b ' + readG7() + ' ' + readG7());

/* ---- CASE 8: a Proxy as globalThis's prototype, so a MISSING global resolves
   through an exotic object. */
var trapLog = [];
var proto = new Proxy({}, {
  has: function (t, k) { if (k === 'g8') { trapLog.push('has:' + k); return true; } return k in t; },
  get: function (t, k) { if (k === 'g8') { trapLog.push('get:' + k); return 'g8-from-proxy'; } return t[k]; }
});
var oldProto = Object.getPrototypeOf(globalThis);
Object.setPrototypeOf(globalThis, proto);
function readG8() { return g8; }
print('8 ' + readG8());
/* The trap SEQUENCE is deliberately not printed.  MEASURED 2026-07-30 on the
   unmodified 0045 engine: node logs `has:g8,get:g8` and quickjs-ng logs
   `get:g8` — quickjs-ng does not run the `has` trap of globalThis's prototype
   for an unresolved global read.  That divergence pre-dates this corpus and is
   unrelated to the early-out; printing it would make this file fail for the
   wrong reason.  The resolved VALUE, which is what the early-out could change,
   is printed and does match. */
Object.setPrototypeOf(globalThis, oldProto);

/* ---- CASE 9: a lexical binding shadowing a name that is ALSO reached through
   the prototype chain of globalThis. */
let g9 = 'lexical-g9';
function readG9() { return g9; }
print('9 ' + readG9());

/* ---- CASE 10: TDZ inside a `with` block, plus `with` introducing a binding
   that shadows a global. */
globalThis.g10 = 'g10-global';
function readG10() { return g10; }
print('10 ' + readG10());
function withShadow() {
  var o = { g10: 'g10-from-with' };
  with (o) { return g10; }
}
print('10b ' + withShadow());
print('10c ' + readG10());

/* ---- CASE 11: direct eval introducing a global `var` binding, read from a
   function compiled before the binding existed. */
function readG11() { return typeof g11 === 'undefined' ? 'undefined' : g11; }
print('11 ' + readG11());
eval('var g11 = "g11-from-eval";');
print('11b ' + readG11());

/* ---- CASE 12: direct eval introducing a *lexical* binding (which is
   eval-scoped, not global) — the global must be unaffected. */
globalThis.g12 = 'g12-global';
function readG12() { return g12; }
eval('let g12 = "g12-from-eval-let"; ');
print('12 ' + readG12());

/* ---- CASE 13: `var` hoisting into globalThis after a site is warm. */
function readG13() { return typeof g13; }
for (var i13 = 0; i13 < 200; i13++) s = readG13();
print('13 ' + s);
globalThis.g13 = 'g13-late';
print('13b ' + readG13() + ' ' + g13);

/* ---- CASE 14: a non-writable globalThis property, written in sloppy mode
   (silently ignored) — exercises JS_SetGlobalVar's slow path. */
Object.defineProperty(globalThis, 'g14', {
  value: 'g14-frozen', writable: false, configurable: true, enumerable: true
});
function writeG14() { g14 = 'changed'; return g14; }
print('14 ' + writeG14());

/* ---- CASE 15: reading a name that exists nowhere. */
function readMissing() { return totallyUndeclaredGlobalName; }
show(readMissing);
print('15b ' + (typeof totallyUndeclaredGlobalName));

/* ---- CASE 16: many distinct global lexical bindings, so global_var_obj's
   shape grows past one property and its hash table is rehashed while warm
   sites read through it. */
let l0 = 0, l1 = 1, l2 = 2, l3 = 3, l4 = 4, l5 = 5, l6 = 6, l7 = 7;
let l8 = 8, l9 = 9, l10 = 10, l11 = 11, l12 = 12, l13 = 13, l14 = 14, l15 = 15;
function sumLexicals() {
  return l0 + l1 + l2 + l3 + l4 + l5 + l6 + l7 +
         l8 + l9 + l10 + l11 + l12 + l13 + l14 + l15;
}
var acc = 0;
for (var i16 = 0; i16 < 200; i16++) acc = sumLexicals();
print('16 ' + acc);

/* ---- CASE 17: a global lexical whose name collides in the shape hash with a
   globalThis property read in the same loop. */
globalThis.mix = 'mix-global';
let mixLex = 'mix-lexical';
function readMixed() { return globalThis.mix + '/' + mixLex + '/' + g1; }
for (var i17 = 0; i17 < 200; i17++) s = readMixed();
print('17 ' + s);
