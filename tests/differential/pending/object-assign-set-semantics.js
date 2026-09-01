/*
 * Object.assign must use Set semantics, object spread must use Define.
 *
 * WHY THIS EXISTS. 0008-object-spread-fast-path added JS_CopyDataPropertiesFast
 * and routed BOTH `{...src}` and Object.assign through it on a shared `setprop`
 * flag, on the stated grounds that the fast path "only accepts targets that
 * cannot have any" setters. It does not: the shape scan inspects the SOURCE for
 * JS_PROP_TMASK, while the target is only checked for class_id, is_exotic,
 * fast_array and extensible. A target carrying an accessor or a read-only
 * property passes all four and then gets written with JS_DefinePropertyValue.
 *
 * Object.assign is specified as Set(to, nextKey, propValue, true), which invokes
 * the target's setter, walks its PROTOTYPE CHAIN for one, and preserves an
 * existing property's attributes. Define does none of those. Spread is
 * CreateDataPropertyOrThrow, so Define is correct there.
 *
 * The two cases test262 caught are the first two below. The rest are the ones it
 * does not cover and which a narrower fix would have left broken.
 */

function show(label, fn) {
  try {
    print(label + ': ' + fn());
  } catch (e) {
    print(label + ': threw ' + e.constructor.name);
  }
}

/* --- the two test262 cases ------------------------------------------------ */

show('non-writable target throws', function () {
  'use strict';
  var target = {};
  Object.defineProperty(target, 'attr', { writable: false });
  Object.assign(target, { attr: 1 });
  return 'no throw, attr=' + target.attr;
});

show('target setter runs', function () {
  var seen = [];
  var target = {};
  Object.defineProperty(target, 'attr', { set: function (v) { seen.push(v); } });
  Object.assign(target, { attr: 1 });
  return 'seen=' + JSON.stringify(seen);
});

/* --- what test262 does not cover, but Set semantics also requires ---------- */

show('setter on the PROTOTYPE chain runs', function () {
  var seen = [];
  var proto = {};
  Object.defineProperty(proto, 'attr', { set: function (v) { seen.push(v); } });
  var target = Object.create(proto);
  Object.assign(target, { attr: 7 });
  return 'seen=' + JSON.stringify(seen) + ' own=' + target.hasOwnProperty('attr');
});

show('existing attributes are preserved, not reset to C_W_E', function () {
  var target = {};
  Object.defineProperty(target, 'attr', {
    value: 0, writable: true, enumerable: false, configurable: false,
  });
  Object.assign(target, { attr: 5 });
  var d = Object.getOwnPropertyDescriptor(target, 'attr');
  return 'value=' + d.value + ' enumerable=' + d.enumerable +
         ' configurable=' + d.configurable;
});

show('getter on the source is invoked once', function () {
  var calls = 0;
  var src = {};
  Object.defineProperty(src, 'attr', {
    enumerable: true, get: function () { calls++; return 3; },
  });
  var target = Object.assign({}, src);
  return 'calls=' + calls + ' value=' + target.attr;
});

/* --- spread keeps Define semantics ---------------------------------------- */

show('spread ignores a prototype setter', function () {
  var seen = [];
  var proto = {};
  Object.defineProperty(proto, 'attr', { set: function (v) { seen.push(v); } });
  var src = { attr: 9 };
  var out = Object.assign(Object.create(proto), {});
  var spread = { ...src };
  return 'spreadAttr=' + spread.attr + ' protoSeen=' + JSON.stringify(seen) +
         ' assignOwn=' + out.hasOwnProperty('attr');
});

show('spread copies only own enumerable properties', function () {
  var proto = { inherited: 1 };
  var src = Object.create(proto);
  src.own = 2;
  Object.defineProperty(src, 'hidden', { value: 3, enumerable: false });
  var out = { ...src };
  return JSON.stringify(Object.keys(out).sort());
});

show('spread of a string boxes it', function () {
  return JSON.stringify({ ...'ab' });
});
