/* Constructor `prototype` read.
 *
 * `js_create_from_ctor_cap` reads `ctor.prototype` on every `new`. The engine
 * reads an instantiated bytecode constructor's own normal data slot directly
 * instead of through JS_GetProperty. This corpus pins every way that read can
 * differ: a reassigned prototype, a non-object prototype (falls back to
 * Object.prototype), an accessor on a constructor, class and derived class,
 * the auto-init case (first read realises the stub), and Reflect.construct with
 * a different new.target. Each case prints whether the instance's prototype is
 * the expected object.
 */

function t(name, v) { print(name + ' = ' + String(v)); }

/* ---------- a plain function constructor ---------- */

function F1() {}
var i1 = new F1();
t('plain', Object.getPrototypeOf(i1) === F1.prototype);
t('own-flag', F1.prototype.constructor === F1);

/* ---------- prototype reassigned between two `new` calls ---------- */

function F2() {}
var p2a = { tag: 'a' };
var p2b = { tag: 'b' };
F2.prototype = p2a;
var i2a = new F2();
F2.prototype = p2b;
var i2b = new F2();
t('reassign-1', Object.getPrototypeOf(i2a) === p2a);
t('reassign-2', Object.getPrototypeOf(i2b) === p2b);
t('reassign-distinct', i2a !== i2b);

/* ---------- prototype set to a non-object ---------- */

function F3() {}
F3.prototype = 42;
var i3 = new F3();
t('nonobject-number', Object.getPrototypeOf(i3) === Object.prototype);
F3.prototype = null;
t('nonobject-null', Object.getPrototypeOf(new F3()) === Object.prototype);
F3.prototype = 'str';
t('nonobject-string', Object.getPrototypeOf(new F3()) === Object.prototype);
F3.prototype = undefined;
t('nonobject-undefined', Object.getPrototypeOf(new F3()) === Object.prototype);

/* ---------- the prototype property itself, replaced ---------- */

/* A bytecode function's `prototype` is writable but not configurable, so it can
   be *replaced* with a value (the case above) but not redefined as an accessor.
   Confirm the descriptor the fast path relies on. */
function F4() {}
t('proto-descriptor', (function () {
  var d = Object.getOwnPropertyDescriptor(F4, 'prototype');
  return d.writable === true && d.enumerable === false &&
         d.configurable === false && 'value' in d;
})());

/* a subclass whose prototype property is replaced by a plain object */
function F5() {}
var fake5 = { tag: 'fake5' };
F5.prototype = fake5;
var i5 = new F5();
t('replaced-value', Object.getPrototypeOf(i5) === fake5);

/* a getter on the *instance* property path is unrelated, but a getter installed
   for a different property must not disturb the prototype read */
function F6() {}
var fake6 = { tag: 'fake6' };
F6.prototype = fake6;
Object.defineProperty(F6, 'other', { get: function () { return 1; }, configurable: true });
t('unrelated-getter', Object.getPrototypeOf(new F6()) === fake6);

/* ---------- class and derived class ---------- */

class C1 {}
class C2 extends C1 {}
var d2 = new C2();
t('class-proto', Object.getPrototypeOf(d2) === C2.prototype);
t('class-chain', d2 instanceof C1);
t('class-ctor', C2.prototype.constructor === C2);
t('derived-super', Object.getPrototypeOf(C2.prototype) === C1.prototype);

/* class expression, anonymous and named */
var C3 = class {};
t('class-expr', Object.getPrototypeOf(new C3()) === C3.prototype);
var C4 = class Named {};
t('class-expr-named', Object.getPrototypeOf(new C4()) === C4.prototype);

/* ---------- the auto-init case: `new` before `prototype` was ever read ---- */

/* A function declaration's `prototype` starts as an AUTOINIT stub. The first
   `new` (or `.prototype` read) realises it; the fast path must not have tried
   to read the stub as a plain slot. */
function F7() {}
var i7 = new F7();            /* first ever touch of F7.prototype, via `new` */
t('autoinout-first', Object.getPrototypeOf(i7) === F7.prototype);
t('autoinout-own', Object.prototype.hasOwnProperty.call(F7, 'prototype'));
t('autoinout-descriptor', (function () {
  var d = Object.getOwnPropertyDescriptor(F7, 'prototype');
  return d.writable && !d.enumerable && !d.configurable;
})());

/* a fresh function whose prototype is read generically first, then `new` */
function F8() {}
var pr8 = F8.prototype;       /* generic read realises it */
t('read-then-new', Object.getPrototypeOf(new F8()) === pr8);

/* ---------- Reflect.construct with a different new.target ---------- */

function Base9() {}
function Other9() {}
var i9 = Reflect.construct(Base9, [], Other9);
t('reflect-newtarget', Object.getPrototypeOf(i9) === Other9.prototype);
t('reflect-instanceof', i9 instanceof Other9 && !(i9 instanceof Base9));

/* Reflect.construct with a bound function as new.target */
function Bound10() {}
var bound10 = Base9.bind(null);
var i10 = Reflect.construct(Base9, [], bound10);
t('reflect-bound-newtarget', Object.getPrototypeOf(i10) === Base9.prototype);

/* ---------- derived constructor returning an object ---------- */

class D11 extends C1 {
  constructor() { super(); return { replaced: true }; }
}
var i11 = new D11();
t('derived-return-object', i11.replaced === true && !(i11 instanceof D11));

/* ---------- `new` on a constructor with more arguments than declared ---- */

function F12(a, b) { this.sum = a + b; }
t('ctor-args', new F12(1, 2).sum === 3);
t('ctor-proto-after-args', Object.getPrototypeOf(new F12(3, 4)) === F12.prototype);

/* ---------- a constructor whose prototype is a proxy ---------- */

function F13() {}
var proxyTarget = { tag: 'proxy' };
F13.prototype = new Proxy(proxyTarget, {});
t('proxy-proto', Object.getPrototypeOf(new F13()) === proxyTarget);

/* ---------- an object-literal method used with `new` ---------- */

var holder = {
  Ctor: function (v) { this.v = v; },
};
var i14 = new holder.Ctor(9);
t('object-method-ctor', i14.v === 9 && Object.getPrototypeOf(i14) === holder.Ctor.prototype);

/* ---------- deleting the prototype property ---------- */

function F15() {}
var realProto = F15.prototype;
delete F15.prototype;          /* the own property goes away */
t('deleted-prototype', Object.getPrototypeOf(new F15()) === Object.prototype);
void realProto;
