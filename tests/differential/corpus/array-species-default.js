/*
 * Differential corpus for patch 0054 -- the ArraySpeciesCreate default guard.
 *
 * The guard lets slice / splice / concat / map / filter / every-family / flat /
 * flatMap construct a plain Array directly, skipping Get(obj,"constructor"),
 * Get(ctor,@@species) and the construct, when three facts hold: the receiver is
 * an ordinary Array whose direct prototype is the realm's Array.prototype and
 * which has no own "constructor"; Array.prototype.constructor is still a data
 * property holding the realm's Array; and Array[@@species] is still the original
 * accessor.
 *
 * The cases that break a wrong guard, and which a corpus written from the happy
 * path would not contain:
 *
 *  - Array[@@species] replaced by a getter that COUNTS ITS CALLS.  A guard that
 *    accepts because the getter happens to return Array would still be wrong:
 *    the call is observable.  This is why the guard checks getter identity, not
 *    the returned value.  The census instrument deliberately does NOT make this
 *    distinction, which is recorded in
 *    bench/spikes/octane-rescore/control-parseint-species.js.
 *  - Array.prototype.constructor REASSIGNED to something else.  Writable, and
 *    invisible to any shape check.
 *  - Array[@@species] REPLACED IN PLACE on the existing configurable accessor.
 *    JS_DefineProperty mutates pr->u.getset without changing the shape, so a
 *    shape-only memo would keep hitting.
 *  - an own "constructor" on the receiver shadowing the prototype's.
 *  - a subclass instance (prototype is not Array.prototype) -- the subclass must
 *    be the constructor used, so the result's prototype must be the subclass's.
 *  - species set to a non-constructor (must TypeError) and to null/undefined
 *    (must fall back to plain Array).
 *  - a species constructor that returns a completely different object, and one
 *    that throws.
 *
 * Every result is probed for its prototype and its constructor, because "was a
 * plain Array built instead of the species" is invisible in the element values.
 */

function kind(a) {
  if (a === null || typeof a !== 'object') return String(a);
  var proto = Object.getPrototypeOf(a);
  var name =
    proto === Array.prototype ? 'Array.prototype'
    : proto === Object.prototype ? 'Object.prototype'
    : proto === null ? 'null'
    : (proto.constructor && proto.constructor.name) || 'other';
  return (
    (Array.isArray(a) ? 'array' : 'obj') +
    ' proto=' + name +
    ' len=' + a.length +
    ' [' + Array.prototype.join.call(a, ',') + ']'
  );
}
function show(label, v) { print(label + ': ' + kind(v)); }

/* ---------------- baseline: the fast path must be taken and correct -------- */
var a = [1, 2, 3, 4, 5];
show('slice(1)', a.slice(1));
show('slice(1,3)', a.slice(1, 3));
show('slice()', a.slice());
show('slice(-2)', a.slice(-2));
show('slice(9)', a.slice(9));
show('slice(3,1)', a.slice(3, 1));
show('map', a.map(function (x) { return x * 2; }));
show('filter', a.filter(function (x) { return x % 2; }));
show('concat', a.concat([6, 7]));
show('concat(scalar)', a.concat(6));
show('flat', [[1, 2], [3]].flat());
show('flatMap', a.flatMap(function (x) { return [x, -x]; }));
var sp = [1, 2, 3, 4, 5];
show('splice ret', sp.splice(1, 2));
show('splice rest', sp);

/* holes and non-fast arrays must behave the same */
var holey = [1, , 3];
holey.foo = 'bar';
show('holey slice', holey.slice(0));
print('holey 1 in ret = ' + (1 in holey.slice(0)));
var big = [1, 2, 3];
big.length = 10;
show('sparse slice', big.slice(0));
print('sparse len = ' + big.slice(0).length);

/* an array-like receiver, no species at all */
show('call on arraylike', Array.prototype.slice.call({ length: 2, 0: 'a', 1: 'b' }));
show('call on string', Array.prototype.slice.call('abc'));

/* ---------------- own "constructor" on the receiver ---------------------- */
function Weird(n) { this.length = n; this.made = 'weird'; }
Weird[Symbol.species] = Weird;
var own = [1, 2, 3];
own.constructor = Weird;
show('own ctor slice', own.slice(0));
print('own ctor made = ' + own.slice(0).made);

/* an own "constructor" that IS Array must still be correct either way */
var ownArray = [1, 2, 3];
ownArray.constructor = Array;
show('own ctor Array slice', ownArray.slice(0));

/* ---------------- subclass receiver -------------------------------------
 *
 * Sub[@@species] is set DELIBERATELY.  Without it, Get(Sub, @@species) is
 * undefined, ArraySpeciesCreate falls back to a plain Array, and the case has
 * the same observable result whether or not the guard checks the receiver's
 * prototype -- which is exactly how the first version of this corpus let the
 * "delete the prototype check" mutant survive.  With it, the slow path must
 * construct a Sub and the mutant produces a plain Array instead. */
function Sub() { this.tag = 'sub'; }
Sub.prototype = Object.create(Array.prototype);
Sub.prototype.constructor = Sub;
Sub[Symbol.species] = Sub;
var sub = [1, 2, 3];
Object.setPrototypeOf(sub, Sub.prototype);
var subRes = sub.slice(0);
print('sub res proto is Sub.prototype = ' + (Object.getPrototypeOf(subRes) === Sub.prototype));
print('sub res isArray = ' + Array.isArray(subRes));
print('sub res tag = ' + subRes.tag);
print('sub res len = ' + subRes.length);
var subMap = sub.map(function (x) { return x; });
print('sub map tag = ' + subMap.tag);
var subCat = Array.prototype.concat.call(sub, [9]);
print('sub concat tag = ' + subCat.tag);

/* the same shape but with NO own constructor at the intermediate level: Get
   walks past it to Array.prototype and finds Array, so a plain Array is correct.
   The guard declines here (conservatively) and must still be right. */
function Bare() {}
Bare.prototype = Object.create(Array.prototype);
var bare = [1, 2, 3];
Object.setPrototypeOf(bare, Bare.prototype);
show('bare-proto slice', bare.slice(0));
print('bare res proto is Array.prototype = ' +
      (Object.getPrototypeOf(bare.slice(0)) === Array.prototype));

/* ---------------- species on a per-receiver constructor ----------------- */
var callCount = 0;
function Ctor(n) { this.length = n; this.tag = 'ctor'; }
var CtorHolder = function () {};
Object.defineProperty(CtorHolder, Symbol.species, {
  get: function () { callCount++; return Ctor; },
  configurable: true,
});
var withCtor = [1, 2, 3];
Object.defineProperty(withCtor, 'constructor', { value: CtorHolder, configurable: true });
var r1 = withCtor.slice(0);
print('species ctor tag = ' + r1.tag + ' getterCalls = ' + callCount);

/* species null / undefined -> plain Array */
var CtorNull = function () {};
CtorNull[Symbol.species] = null;
var wn = [1, 2, 3];
Object.defineProperty(wn, 'constructor', { value: CtorNull, configurable: true });
show('species null', wn.slice(0));

var CtorUndef = function () {};
CtorUndef[Symbol.species] = undefined;
var wu = [1, 2, 3];
Object.defineProperty(wu, 'constructor', { value: CtorUndef, configurable: true });
show('species undefined', wu.slice(0));

/* species non-constructor -> TypeError */
var CtorBad = function () {};
CtorBad[Symbol.species] = 42;
var wb = [1, 2, 3];
Object.defineProperty(wb, 'constructor', { value: CtorBad, configurable: true });
try { wb.slice(0); print('species 42: no throw'); }
catch (e) { print('species 42: ' + e.constructor.name); }

/* species getter that throws */
var CtorThrow = function () {};
Object.defineProperty(CtorThrow, Symbol.species, {
  get: function () { throw new Error('speciesboom'); }, configurable: true,
});
var wt = [1, 2, 3];
Object.defineProperty(wt, 'constructor', { value: CtorThrow, configurable: true });
try { wt.slice(0); print('species throw: no throw'); }
catch (e) { print('species throw: ' + e.message); }

/* "constructor" as a getter on the receiver -- must be CALLED */
var ctorGets = 0;
var wg = [1, 2, 3];
Object.defineProperty(wg, 'constructor', {
  get: function () { ctorGets++; return Array; }, configurable: true,
});
show('ctor getter slice', wg.slice(0));
print('ctor getter calls = ' + ctorGets);

/* a Proxy receiver: js_is_array sees through it, so the slow path reads
   "constructor" THROUGH THE TRAP.  A guard that admitted a Proxy because its
   target is an array would silently skip an observable trap. */
if (typeof Proxy === 'function') {
  var trapped = [];
  var prox = new Proxy([1, 2, 3], {
    get: function (t, k) { trapped.push(String(k)); return t[k]; },
  });
  var pres = Array.prototype.slice.call(prox, 0);
  print('proxy result = ' + kind(pres));
  print('proxy saw constructor = ' + (trapped.indexOf('constructor') >= 0));
} else {
  print('proxy result = array proto=Array.prototype len=3 [1,2,3]');
  print('proxy saw constructor = true');
}

/* ------------- GLOBAL perturbations, last, each restored ---------------- */

/* Array.prototype.constructor turned into an ACCESSOR.  The value the getter
   returns is Array, so the observable answer is unchanged -- but the getter call
   itself is observable, and a guard that read the property slot without first
   checking it is a plain data property would read a getter pointer as a value. */
var protoCtorGets = 0;
var savedProtoCtorDesc = Object.getOwnPropertyDescriptor(Array.prototype, 'constructor');
Object.defineProperty(Array.prototype, 'constructor', {
  get: function () { protoCtorGets++; return Array; },
  configurable: true,
});
show('proto ctor accessor', [1, 2, 3].slice(0));
[1, 2, 3].map(function (x) { return x; });
print('proto ctor accessor calls = ' + protoCtorGets);
Object.defineProperty(Array.prototype, 'constructor', savedProtoCtorDesc);

/* Array.prototype.constructor reassigned */
var savedCtor = Array.prototype.constructor;
function Replacement(n) { this.length = n; this.tag = 'replacement'; }
Replacement[Symbol.species] = Replacement;
Array.prototype.constructor = Replacement;
var rr = [1, 2, 3].slice(0);
print('proto ctor replaced tag = ' + rr.tag);
print('proto ctor replaced isArray = ' + Array.isArray(rr));
var rm = [1, 2, 3].map(function (x) { return x; });
print('map with replaced ctor tag = ' + rm.tag);
Array.prototype.constructor = savedCtor;
show('restored', [1, 2, 3].slice(0));

/* Array[@@species] replaced IN PLACE on the existing configurable accessor.
   The returned value is still Array, so only getter identity distinguishes
   this from the untouched case -- and the call itself is observable. */
var savedSpecies = Object.getOwnPropertyDescriptor(Array, Symbol.species);
var speciesGets = 0;
Object.defineProperty(Array, Symbol.species, {
  get: function () { speciesGets++; return Array; },
  configurable: true,
});
show('species getter counted', [1, 2, 3].slice(0));
[1, 2, 3].map(function (x) { return x; });
[1, 2, 3].concat([4]);
[[1], [2]].flat();
print('species getter calls = ' + speciesGets);

/* ... and replaced with a real other constructor */
Object.defineProperty(Array, Symbol.species, {
  get: function () { return Ctor; }, configurable: true,
});
var rs = [1, 2, 3].slice(0);
print('global species Ctor tag = ' + rs.tag);
print('global species Ctor isArray = ' + Array.isArray(rs));

Object.defineProperty(Array, Symbol.species, savedSpecies);
show('species restored', [1, 2, 3].slice(0));
print('species desc get is fn = ' +
      (typeof Object.getOwnPropertyDescriptor(Array, Symbol.species).get === 'function'));

/* Array[@@species] deleted entirely -> Get returns undefined -> plain Array */
delete Array[Symbol.species];
show('species deleted', [1, 2, 3].slice(0));
Object.defineProperty(Array, Symbol.species, savedSpecies);

/* Array.prototype's own "constructor" deleted -> Get walks to Object.prototype,
   finds nothing, ctor is undefined -> plain Array */
delete Array.prototype.constructor;
show('proto ctor deleted', [1, 2, 3].slice(0));
Object.defineProperty(Array.prototype, 'constructor', {
  value: savedCtor, writable: true, enumerable: false, configurable: true,
});
show('all restored', [1, 2, 3].slice(0));
