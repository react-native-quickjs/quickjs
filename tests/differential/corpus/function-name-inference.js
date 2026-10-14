/* Function `name` inference.
 *
 * `set_name` replaces the empty name a closure is created with by its inferred
 * one. The engine does that in place when the function already owns a
 * configurable, non-writable, non-enumerable string `name` property, instead of
 * calling the generic property definer. This corpus exercises every inference
 * shape and the descriptor, and the register build, the stack build and node
 * must produce identical output.
 *
 * The subtle ones: a *named function expression* assigned to a differently
 * named variable keeps its own name (the `name` is not empty, so `set_name`
 * does nothing); and a `name` redefined with Object.defineProperty before the
 * assignment must not be clobbered.
 */

function show(v) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (v === undefined) return 'undefined';
  if (v === null) return 'null';
  if (typeof v === 'object') {
    var ks = Object.keys(v), i, out = [];
    for (i = 0; i < ks.length; i++) out.push(ks[i] + ':' + show(v[ks[i]]));
    return '{' + out.join(',') + '}';
  }
  return String(v);
}
function t(name, v) { print(name + ' = ' + show(v)); }
function desc(f) {
  var d = Object.getOwnPropertyDescriptor(f, 'name');
  if (!d) return '<none>';
  return { value: d.value, writable: d.writable,
           enumerable: d.enumerable, configurable: d.configurable };
}

/* ---------- the basic inference shapes ---------- */

var a1 = function () {};
t('var-anon', desc(a1));

let a2 = function () {};
t('let-anon', desc(a2));

const a3 = function () {};
t('const-anon', desc(a3));

a4 = function () {};
t('assign-anon', desc(a4));

var a5 = () => {};
t('arrow-var', desc(a5));

let a6 = () => {};
t('arrow-let', desc(a6));

const a7 = async function () {};
t('async-fn', desc(a7));

function* a8() {}
t('gen-decl', desc(a8));

var a9 = function* () {};
t('gen-expr', desc(a9));

var a10 = class {};
t('class-var', desc(a10));

let a11 = class {};
t('class-let', desc(a11));

/* ---------- object-literal methods and properties ---------- */

var obj = {
  m1: function () {},
  m2() {},
  get g1() { return 1; },
  set s1(v) {},
  arrow: () => {},
  async m3() {},
  *m4() {},
};
t('obj-m1', desc(obj.m1));
t('obj-m2', desc(obj.m2));
t('obj-g1', desc(Object.getOwnPropertyDescriptor(obj, 'g1').get));
t('obj-arrow', desc(obj.arrow));
t('obj-m3', desc(obj.m3));
t('obj-m4', desc(obj.m4));

/* computed method names */
var key = 'dyn';
var obj2 = {
  [key]: function () {},
  [key + '2']() {},
};
t('obj-dyn', desc(obj2.dyn));
t('obj-dyn2', desc(obj2.dyn2));

/* ---------- a named function expression keeps its own name ---------- */

var b1 = function inner() {};
t('named-keeps', desc(b1));

let b2 = function inner2() {};
t('named-let', desc(b2));

var b3 = function inner3() { return inner3; };
t('named-selfref', b3() === b3);

/* a named function expression whose own name was never used still keeps it */
t('named-not-clobbered', (function named() {}).name);

/* ---------- delete f.name then read it ---------- */

var c1 = function () {};
delete c1.name;
t('deleted-name', desc(c1));
t('deleted-name-value', c1.name);

/* delete then re-read after an unrelated assignment */
var c2 = function () {};
delete c2.name;
var c2b = c2;
t('deleted-again', desc(c2b));

/* ---------- redefined name is not clobbered ---------- */

var d0 = function () {};
Object.defineProperty(d0, 'name', { value: 'fixed', configurable: true });
t('redef-before', desc(d0));
/* assigning it to another variable re-runs set_name with the NEW name; the
   existing non-empty string must win (JS_DefineObjectName is a no-op) */
var d1 = d0;
t('redef-kept', desc(d1));

/* a non-configurable name survives too */
var d2 = function () {};
Object.defineProperty(d2, 'name', { value: 'locked', configurable: false });
var d3 = d2;
t('nonconfig', desc(d3));

/* a name redefined as a getter/accessor: the fast path must defer to the
   definer, which refuses to touch it (TMASK != NORMAL) */
var d4 = function () {};
Object.defineProperty(d4, 'name', { get: function () { return 'via-getter'; }, configurable: true });
t('getter-name', d4.name);
var d5 = d4;
t('getter-name-after-assign', d5.name);

/* a name defined as a non-string value (number) */
var d6 = function () {};
Object.defineProperty(d6, 'name', { value: 42, configurable: true });
var d7 = d6;
t('numeric-name', desc(d7));

/* ---------- class expressions and derived classes ---------- */

var e1 = class Named { };
t('class-named', desc(e1));
var e2 = class extends Object { };
t('class-derived-anon', typeof e2.name === 'string' && e2.name.length > 0 ? '<named>' : e2.name);
var e3 = class NamedDerived extends Object { };
t('class-derived-named', desc(e3));

/* ---------- nested / chained inference ---------- */

var f1 = function () {
  return function () {};
};
t('nested-inner', desc(f1()));

var f2 = { wrap: function () { return function () {}; } };
t('nested-method-inner', desc(f2.wrap()));

/* inference through a default parameter and a destructuring assignment */
function g1(cb = function () {}) { return desc(cb); }
t('default-param', g1());

var g2 = function () {};
[g2].forEach(function (x) { t('foreach-kept', desc(x)); });

/* ---------- name of functions created by engine helpers ---------- */

t('bound-name', String(function target() {}.bind(null).name));
t('arrow-in-object', desc({ f: () => {} }.f));

/* ---------- the whole descriptor printed in full ---------- */

t('full-descriptor', Object.getOwnPropertyDescriptor(a1, 'name'));
t('indexOf-name', Object.getOwnPropertyNames(a1).indexOf('name') >= 0);
