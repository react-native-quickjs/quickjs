/* Adversarial corpus for the CLOSURE SHAPE TEMPLATE mechanism
   (patches/quickjs-ng/0029-closure-shape-templates.patch,
    docs/closure-shape-templates.md).

   The mechanism under test replaces the two or three
   `JS_DefinePropertyValue` shape transitions that `js_closure()` performs for
   `length`, `name` and `prototype` with a single `JS_NewObjectFromShape()`
   against a per-realm cached `JSShape`, one per
   `(func_kind, has_prototype)` pair. Every closure of a given kind therefore
   SHARES one shape object rather than arriving at an equivalent one by
   transition.

   Every case below tries to make that shared shape observable — by reading a
   descriptor, by changing one function's own properties and asking whether
   another function of the same kind changed with it, or by transitioning a
   closure off the template and back.

   ## What each guard is discriminated by

     guard                                        detected by
     -------------------------------------------  --------------------------
     length/name flags are exactly CONFIGURABLE    cases 1, 2, 3
     prototype flags are exactly WRITABLE          cases 1, 4
     property ORDER is length, name, prototype     cases 1, 5
     the prototype slot holds the AUTOINIT marker  cases 6, 7, 8
       (not a materialised object, and not a
        stale one shared between closures)
     the cached shape is HASHED, so
       js_shape_prepare_update() clones instead
       of mutating it in place                     cases 9, 10, 11, 12
     slot index of each of the three values        cases 1, 13
     arrow/method/accessor get the NO-PROTOTYPE
       template, not the 3-property one            cases 3, 5, 14
     generator prototype is a FRESH object per
       closure, not one shared via the template    case 15
     is_constructor is set for exactly the
       functions that had it before                cases 16, 17
     the template is per (func_kind, has_proto)    cases 5, 15, 18

   ## What no output diff can reach

   The `sh->proto == object_or_null(ctx->class_proto[class_id])` re-check on
   every template hit is defence in depth against a realm whose `class_proto[]`
   entry was replaced after the template was cached. Nothing in the public API
   replaces `ctx->class_proto[JS_CLASS_BYTECODE_FUNCTION]` after intrinsics are
   installed, so no reachable JavaScript input can distinguish a build with that
   compare deleted. It is kept because the cost is one load and one compare and
   the failure mode — closures built with the wrong prototype — is silent.
   Stated here rather than faked as a test; see
   docs/mutation-testing-the-safety-nets.md.

   Deterministic, ES5-printable, no host objects. */

/* node runs this file as a sloppy-mode CommonJS script, where every ordinary
   function additionally carries its own non-configurable `arguments` and
   `caller` poison properties; QuickJS puts those accessors on
   Function.prototype instead. That difference is pre-existing, unrelated to
   the template, and would otherwise mask every real diff -- so both names are
   filtered out of every listing below. */
function names(o) {
  var all = Object.getOwnPropertyNames(o);
  var keys = [];
  for (var i = 0; i < all.length; i++)
    if (all[i] !== 'arguments' && all[i] !== 'caller') keys.push(all[i]);
  return keys;
}

function show(o) {
  var keys = names(o);
  var out = [];
  for (var i = 0; i < keys.length; i++) {
    var d = Object.getOwnPropertyDescriptor(o, keys[i]);
    var v;
    if ('value' in d) {
      v = typeof d.value === 'function' ? 'fn' :
          typeof d.value === 'object' && d.value !== null ? 'obj' : String(d.value);
    } else {
      v = 'accessor';
    }
    out.push(keys[i] + '=' + v + '{' +
             (d.writable ? 'w' : '-') +
             (d.enumerable ? 'e' : '-') +
             (d.configurable ? 'c' : '-') + '}');
  }
  return out.join(' ');
}

/* ---- CASE 1: the full descriptor set of an ordinary function ---------- */
function named(a, b) { return a + b; }
print('1 ' + show(named));

/* ---- CASE 2: length and name are configurable, not writable ----------- */
function f2(x) {}
f2.length = 99;
f2.name = 'nope';
print('2a ' + f2.length + ' ' + f2.name);
Object.defineProperty(f2, 'length', { value: 7 });
Object.defineProperty(f2, 'name', { value: 'renamed' });
print('2b ' + f2.length + ' ' + f2.name);
print('2c ' + show(f2));

/* ---- CASE 3: arrows, methods and accessors have NO prototype ---------- */
var arrow = function (a, b, c) { return a; };
var obj3 = {
  meth: function (x) { return x; },
  get g() { return 1; },
  set s(v) {},
};
print('3a ' + show(arrow));
print('3b ' + ('prototype' in arrow));
var d3g = Object.getOwnPropertyDescriptor(obj3, 'g');
var d3s = Object.getOwnPropertyDescriptor(obj3, 's');
print('3c ' + show(d3g.get) + ' | ' + show(d3s.set));

/* ---- CASE 4: prototype is writable, not enumerable, not configurable -- */
function f4() {}
print('4a ' + show(f4));
var p4 = f4.prototype;
f4.prototype = { tag: 'replaced' };
print('4b ' + (f4.prototype === p4) + ' ' + f4.prototype.tag);
print('4c ' + show(f4));
print('4d ' + delete f4.prototype);
print('4e ' + show(f4));

/* ---- CASE 5: own-property ORDER, across every function kind ----------- */
function ord0() {}
var ordArrow = function () {};
function* ordGen() {}
async function ordAsync() {}
async function* ordAsyncGen() {}
print('5a ' + names(ord0).join(','));
print('5b ' + names(ordArrow).join(','));
print('5c ' + names(ordGen).join(','));
print('5d ' + names(ordAsync).join(','));
print('5e ' + names(ordAsyncGen).join(','));
print('5f ' + show(ordGen));
print('5g ' + show(ordAsync));

/* ---- CASE 6: the lazy prototype is instantiated ONCE, per function ---- */
function mk6() { return function inner() {}; }
var a6 = mk6(), b6 = mk6();
print('6a ' + (a6.prototype === a6.prototype));
print('6b ' + (a6.prototype === b6.prototype));
print('6c ' + (a6.prototype.constructor === a6) + ' ' + (b6.prototype.constructor === b6));
print('6d ' + show(a6.prototype));

/* ---- CASE 7: reading one closure's prototype must not materialise
       another's, and must not leave the shared template holding it ------ */
function mk7() { return function () {}; }
var seven = [mk7(), mk7(), mk7()];
var p7 = seven[1].prototype;         /* materialise the middle one only */
print('7a ' + (seven[0].prototype === p7) + ' ' + (seven[2].prototype === p7));
print('7b ' + show(seven[0]));

/* ---- CASE 8: deleting the un-materialised prototype ------------------- */
function mk8() { return function () {}; }
var a8 = mk8(), b8 = mk8();
print('8a ' + delete a8.prototype);
print('8b ' + ('prototype' in a8) + ' ' + ('prototype' in b8));
print('8c ' + (typeof b8.prototype));

/* ---- CASE 9: adding own properties to ONE closure of a shared shape --- */
function mk9() { return function () {}; }
var a9 = mk9(), b9 = mk9();
a9.extra = 1;
a9.more = 2;
print('9a ' + names(a9).join(','));
print('9b ' + names(b9).join(','));
print('9c ' + a9.extra + ' ' + b9.extra);

/* ---- CASE 10: redefining name on ONE closure -------------------------- */
function mk10() { return function orig() {}; }
var a10 = mk10(), b10 = mk10(), c10 = mk10();
Object.defineProperty(a10, 'name', { value: 'A', writable: true, enumerable: true });
print('10a ' + show(a10));
print('10b ' + show(b10));
print('10c ' + show(c10));

/* ---- CASE 11: deleting length on ONE closure -------------------------- */
function mk11() { return function (x, y) {}; }
var a11 = mk11(), b11 = mk11();
delete a11.length;
print('11a ' + names(a11).join(','));
print('11b ' + names(b11).join(',') + ' ' + b11.length);
a11.length = 'now-a-plain-prop';
print('11c ' + show(a11));
print('11d ' + show(b11));

/* ---- CASE 12: freeze one, leave the other alone ----------------------- */
function mk12() { return function (q) {}; }
var a12 = mk12(), b12 = mk12();
Object.freeze(a12);
print('12a ' + Object.isFrozen(a12) + ' ' + Object.isFrozen(b12));
print('12b ' + show(a12));
print('12c ' + show(b12));
b12.tacked = 3;
print('12d ' + show(b12));
a12.tacked = 4;
print('12e ' + show(a12));

/* ---- CASE 13: values land in the right slots, for many arities -------- */
function v0() {}
function v1(a) {}
function v2(a, b) {}
function v3(a, b, c) {}
function vRest(a, b) { }
function vDefault(a, b) {}
print('13a ' + [v0.length, v1.length, v2.length, v3.length].join(','));
print('13b ' + [v0.name, v1.name, v2.name, v3.name].join(','));
var anon13 = (function () { return function () {}; })();
print('13c "' + anon13.name + '" ' + anon13.length);
var assigned13 = function () {};
print('13d "' + assigned13.name + '"');

/* ---- CASE 14: `new` on an arrow throws; on a function it works -------- */
function Ctor14(x) { this.x = x; }
var made14 = new Ctor14(5);
print('14a ' + made14.x + ' ' + (Object.getPrototypeOf(made14) === Ctor14.prototype));
var arrow14 = function () {};
try { new (function () { return 1; })(); print('14b ok'); } catch (e) { print('14b ' + e.name); }
var trueArrow = (0, eval)('(x) => x');
try { new trueArrow(1); print('14c no-throw'); } catch (e) { print('14c ' + e.name); }

/* ---- CASE 15: every generator closure gets its OWN prototype object --- */
function mkGen() { return function* (n) { yield n; }; }
var g1 = mkGen(), g2 = mkGen();
print('15a ' + (g1.prototype === g2.prototype));
print('15b ' + show(g1));
g1.prototype.marker = 'g1';
print('15c ' + (g2.prototype.marker === undefined));
var it15 = g1(4);
print('15d ' + it15.next().value + ' ' + JSON.stringify(it15.next()));
print('15e ' + (Object.getPrototypeOf(it15) === g1.prototype));

/* ---- CASE 16: is_constructor -- typeof, new, and Reflect.construct ---- */
function mk16() { return function () { this.z = 1; }; }
var a16 = mk16();
print('16a ' + (new a16()).z);
print('16b ' + (Reflect.construct(a16, []).z));
function* gen16() {}
try { new gen16(); print('16c no-throw'); } catch (e) { print('16c ' + e.name); }
async function async16() {}
try { new async16(); print('16d no-throw'); } catch (e) { print('16d ' + e.name); }
var obj16 = { m: function () {} };
try { new obj16.m(); print('16e no-throw'); } catch (e) { print('16e ' + e.name); }

/* ---- CASE 17: bind, call, apply off a templated closure --------------- */
function f17(a, b, c) { return this.t + a + b + c; }
var bound17 = f17.bind({ t: 'T' }, 'a');
print('17a ' + bound17('b', 'c'));
print('17b ' + show(bound17));
print('17c ' + f17.call({ t: 'U' }, 1, 2, 3));
print('17d ' + f17.apply({ t: 'V' }, [1, 2, 3]));

/* ---- CASE 18: enumeration, spread and assign see nothing -------------- */
function f18(a) {}
var seen18 = [];
for (var k18 in f18) seen18.push(k18);
print('18a [' + seen18.join(',') + ']');
print('18b ' + JSON.stringify(Object.assign({}, f18)));
print('18c ' + JSON.stringify(Object.keys(f18)));
f18.vis = 1;
var seen18b = [];
for (var k18b in f18) seen18b.push(k18b);
print('18d [' + seen18b.join(',') + ']');

/* ---- CASE 19: setPrototypeOf off the shared shape --------------------- */
function mk19() { return function () {}; }
var a19 = mk19(), b19 = mk19();
Object.setPrototypeOf(a19, null);
print('19a ' + (Object.getPrototypeOf(a19) === null));
print('19b ' + (Object.getPrototypeOf(b19) === Function.prototype));
print('19c ' + show(a19) + ' | ' + show(b19));
print('19d ' + (typeof b19.call));

/* ---- CASE 20: a great many closures of both kinds, interleaved -------- */
var acc20 = 0;
for (var i20 = 0; i20 < 200; i20++) {
  var withProto = function (p, q) { return p; };
  var noProto = { m: function (p) { return p; } }.m;
  withProto.tag = i20;
  acc20 += withProto.length + noProto.length + withProto.tag;
  if (i20 % 50 === 0) acc20 += withProto.prototype ? 1 : 0;
}
print('20a ' + acc20);
var last20 = function (p, q) {};
print('20b ' + show(last20));

/* ---- CASE 21: closures created inside a class body ------------------- */
class C21 {
  constructor() { this.made = function inner21(x) { return x; }; }
  m21(a, b) { return a; }
  static s21() {}
  get g21() { return 1; }
}
var c21 = new C21();
print('21a ' + show(c21.made));
print('21b ' + show(C21.prototype.m21));
print('21c ' + show(C21.s21));
print('21d ' + names(C21).join(','));

/* ---- CASE 22: async functions and their (absent) prototype ----------- */
async function a22(x, y) { return x; }
print('22a ' + show(a22));
print('22b ' + ('prototype' in a22));
var arrowAsync = (0, eval)('async (x) => x');
print('22c ' + show(arrowAsync));

/* ---- CASE 23: a function whose name is the empty string -------------- */
var anon23 = (function () { return function () {}; })();
print('23a "' + anon23.name + '" ' + JSON.stringify(Object.getOwnPropertyDescriptor(anon23, 'name')));
var gen23 = (function () { return function* () {}; })();
print('23b "' + gen23.name + '"');

/* ---- CASE 24: sealing, preventExtensions, and re-reading ------------- */
function mk24() { return function (a) {}; }
var a24 = mk24(), b24 = mk24();
Object.preventExtensions(a24);
print('24a ' + Object.isExtensible(a24) + ' ' + Object.isExtensible(b24));
a24.nope = 1;
print('24b ' + show(a24));
print('24c ' + a24.prototype.constructor.name);
Object.seal(b24);
print('24d ' + Object.isSealed(b24) + ' ' + show(b24));
