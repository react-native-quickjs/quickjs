/*
 * Copyright (c) Ammar Ahmed.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/*
 * The ECMAScript surface outside the React/RN hot loop.
 *
 * The other workloads model programs. This one models *the library and language
 * surface itself*, one operation per row, chosen because a systematic sweep
 * found a concentrated gap there (or, for the guard rows, a concentrated lead
 * worth protecting). See docs/ecmascript-gap-sweep.md for how each row was
 * selected and what the numbers meant on the day they were taken.
 *
 * Two rules this file follows and any addition must follow too:
 *
 *  1. Anything allocated is pushed into a module-level array that outlives the
 *     iteration. Hermes runs a real optimizer and will scalar-replace an
 *     object whose lifetime it can bound, which turns an allocation benchmark
 *     into a measurement of nothing. The escape is not decoration.
 *
 *  2. Cheap operations run in an inner repeat loop, with the count in `unit`,
 *     so the row measures the operation rather than the harness's own call and
 *     blackhole overhead.
 *
 * The `floor/*` rows are calibration, not targets. They are the cost of the
 * interpreter's own loop, call and property-read on each engine, and every
 * other ratio in this file has to be read against them: a row at the same
 * ratio as the floor is not a finding about that operation, it is the
 * dispatch gap showing through.
 */

/* --------------------------------------------------- dispatch-floor calibration */

var floorObj = { a: 1 };
var floorArr = (function () { var a = new Array(200); for (var i = 0; i < 200; i++) a[i] = 1; return a; })();
function floorCall1(x) { return x; }
var floorRecvA = {}, floorRecvB = {};

bench({
  name: 'floor/add-loop-200', unit: '200 adds',
  run: function () { var s = 0; for (var i = 0; i < 200; i++) s += i; return s; },
  expect: 19900,
});
bench({
  name: 'floor/field-read-loop-200', unit: '200 reads',
  run: function () { var o = floorObj, s = 0; for (var i = 0; i < 200; i++) s += o.a; return s; },
  expect: 200,
});
bench({
  name: 'floor/array-index-loop-200', unit: '200 loads',
  run: function () { var a = floorArr, s = 0; for (var i = 0; i < 200; i++) s += a[i]; return s; },
  expect: 200,
});
bench({
  name: 'floor/call-1arg-loop-200', unit: '200 calls',
  run: function () { var s = 0; for (var i = 0; i < 200; i++) s += floorCall1(1); return s; },
  expect: 200,
});
bench({
  name: 'floor/strict-eq-loop-200', unit: '200 compares',
  run: function () { var a = floorRecvA, b = floorRecvB, s = 0; for (var i = 0; i < 200; i++) s += (a === b) ? 0 : 1; return s; },
  expect: 200,
});

/* ------------------------------------------------------------------ allocation */

/* Everything allocated here escapes into KEEP and is only dropped in bulk, so
   neither engine can prove the allocation dead. */
var KEEP = [];
function keepReset() { KEEP.length = 0; }
function keepPush(v) { KEEP.push(v); if (KEEP.length > 5000) KEEP.length = 0; }

bench({
  name: 'alloc/closure-0cap', unit: '50 closures', before: keepReset,
  run: function () { for (var i = 0; i < 50; i++) keepPush(function () { return 1; }); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});
bench({
  name: 'alloc/closure-1cap', unit: '50 closures', before: keepReset,
  run: function () { for (var i = 0; i < 50; i++) { var c = i; keepPush(function () { return c; }); } return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});
bench({
  name: 'alloc/obj-literal-7key', unit: '50 objects', before: keepReset,
  run: function () { for (var i = 0; i < 50; i++) keepPush({ a: i, b: i, c: i, d: i, e: i, f: i, g: i }); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});
/* 17 keys is one past the object-literal shape template's eligibility limit.
   Paired with the 16-key row it makes the coverage cliff visible; if the two
   ever converge, the cap moved. */
bench({
  name: 'alloc/obj-literal-16key', unit: '50 objects', before: keepReset,
  run: function () { for (var i = 0; i < 50; i++) keepPush({ k0: i, k1: i, k2: i, k3: i, k4: i, k5: i, k6: i, k7: i, k8: i, k9: i, ka: i, kb: i, kc: i, kd: i, ke: i, kf: i }); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});
bench({
  name: 'alloc/obj-literal-17key', unit: '50 objects', before: keepReset,
  run: function () { for (var i = 0; i < 50; i++) keepPush({ k0: i, k1: i, k2: i, k3: i, k4: i, k5: i, k6: i, k7: i, k8: i, k9: i, ka: i, kb: i, kc: i, kd: i, ke: i, kf: i, kg: i }); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});

/* ------------------------------------------------------------- array built-ins */

var arr100 = (function () { var a = new Array(100); for (var i = 0; i < 100; i++) a[i] = i; return a; })();
var objs100 = (function () { var a = []; for (var i = 0; i < 100; i++) a.push({ id: i }); return a; })();
var needle80 = objs100[80];

bench({
  name: 'array/concat-two-100', unit: 'concat',
  run: function () { return arr100.concat(arr100).length; },
  expect: 200,
});
bench({
  name: 'array/slice-copy-100', unit: 'copy',
  run: function () { return arr100.slice().length; },
  expect: 100,
});
bench({
  name: 'array/indexOf-miss-100', unit: 'scan',
  run: function () { return arr100.indexOf(-1); },
  expect: -1,
});
bench({
  name: 'array/includes-miss-100', unit: 'scan',
  run: function () { return arr100.includes(-1) ? 1 : 0; },
  expect: 0,
});
bench({
  name: 'array/indexOf-object-100', unit: 'scan',
  run: function () { return objs100.indexOf(needle80); },
  expect: 80,
});
bench({
  name: 'array/splice-insert-mid-100', unit: 'splice',
  run: function () { var a = arr100.slice(); a.splice(50, 0, 1, 2, 3); return a.length; },
  expect: 103,
});

/* --------------------------------------------------------------- object statics */

var obj7 = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7 };
var hasOwn = Object.prototype.hasOwnProperty;

bench({
  name: 'object/hasOwnProperty-call-7', unit: '7 checks',
  run: function () {
    var n = 0;
    if (hasOwn.call(obj7, 'a')) n++;
    if (hasOwn.call(obj7, 'b')) n++;
    if (hasOwn.call(obj7, 'c')) n++;
    if (hasOwn.call(obj7, 'd')) n++;
    if (hasOwn.call(obj7, 'zz')) n++;
    if (hasOwn.call(obj7, 'yy')) n++;
    if (hasOwn.call(obj7, 'g')) n++;
    return n;
  },
  expect: 5,
});
bench({
  name: 'object/entries-7', unit: 'call',
  run: function () { return Object.entries(obj7).length; },
  expect: 7,
});
bench({
  name: 'object/freeze-7', unit: 'freeze',
  run: function () { var t = { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7 }; Object.freeze(t); return Object.isFrozen(t) ? 1 : 0; },
  expect: 1,
});
/* RN's module system runs Object.defineProperty on every ES-module interop
   boundary, so this is on the startup path of every bundle. */
bench({
  name: 'object/defineProperty-3', unit: '3 defines',
  run: function () {
    var t = {};
    Object.defineProperty(t, 'a', { value: 1, enumerable: true });
    Object.defineProperty(t, 'b', { value: 2, enumerable: true });
    Object.defineProperty(t, '__esModule', { value: true });
    return t.b;
  },
  expect: 2,
});

/* ------------------------------------------------------------------- exceptions */

/* The whole cost of a throw in quickjs is the stack trace built on the way out,
   and it is built even when the thrown value is a primitive that can never
   carry one. React throws a thenable to suspend, which is exactly that case. */
var suspender = { then: function () {} };
function throwInt() { throw 1; }
function throwInt5() { (function () { (function () { (function () { throwInt(); })(); })(); })(); }
function throwError() { throw new Error('boom'); }

bench({
  name: 'exc/throw-int-depth5', unit: '20 throws',
  run: function () { var s = 0; for (var i = 0; i < 20; i++) { try { throwInt5(); } catch (e) { s += e; } } return s; },
  expect: 20,
});
bench({
  name: 'exc/throw-thenable', unit: '20 throws',
  run: function () { var s = 0; for (var i = 0; i < 20; i++) { try { throw suspender; } catch (e) { s += (typeof e.then === 'function') ? 1 : 0; } } return s; },
  expect: 20,
});
bench({
  name: 'exc/throw-Error', unit: '20 throws',
  run: function () { var s = 0; for (var i = 0; i < 20; i++) { try { throwError(); } catch (e) { s += e.message.length; } } return s; },
  expect: 80,
});
bench({
  name: 'exc/new-Error-no-throw', unit: '20 allocations', before: keepReset,
  run: function () { for (var i = 0; i < 20; i++) keepPush(new Error('boom')); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});

/* --------------------------------------------------------- misc surface, weighted */

var switchKeys = ['top', 'left', 'width', 'height', 'color', 'flex'];
bench({
  name: 'ctl/switch-string-6', unit: '200 switches',
  run: function () {
    var s = 0;
    for (var i = 0; i < 200; i++) {
      switch (switchKeys[i % 6]) {
        case 'top': s += 1; break;
        case 'left': s += 2; break;
        case 'width': s += 3; break;
        case 'height': s += 4; break;
        case 'color': s += 5; break;
        default: s += 6;
      }
    }
    return s;
  },
  expect: 696,
});
function argsSum() { var r = 0; for (var i = 0; i < arguments.length; i++) r += arguments[i]; return r; }
bench({
  name: 'fn/arguments-object-4', unit: '200 calls',
  run: function () { var s = 0; for (var i = 0; i < 200; i++) s += argsSum(1, 2, 3, 4); return s; },
  expect: 2000,
});

var fixedDate = new Date(1700000000000);
bench({
  name: 'date/field-getters', unit: '20 x 4 getters',
  run: function () {
    var d = fixedDate, s = 0;
    for (var i = 0; i < 20; i++) s += d.getUTCFullYear() + d.getUTCMonth() + d.getUTCDate() + d.getUTCHours();
    return s;
  },
  expect: 41380,
});

var i32 = (function () { var t = new Int32Array(256); for (var i = 0; i < 256; i++) t[i] = i; return t; })();
bench({
  name: 'typedarray/i32-read-256', unit: 'scan',
  run: function () { var s = 0; for (var i = 0; i < 256; i++) s += i32[i]; return s; },
  expect: 32640,
});

var char40 = 'the quick brown fox jumps over lazydog12';
bench({
  name: 'str/split-empty-40', unit: 'split',
  run: function () { return char40.split('').length; },
  expect: 40,
});

/* ------------------------------------------------------------------- guard rows */

/* These are places quickjs is ahead. They are here so a future change that
   trades them away shows up as a regression instead of disappearing quietly. */

function* countTo(n) { for (var i = 0; i < n; i++) yield i; }
bench({
  name: 'guard/generator-100', unit: 'drain',
  run: function () { var s = 0; for (var x of countTo(100)) s += x; return s; },
  expect: 4950,
});
bench({
  name: 'guard/array-from-iterable-100', unit: 'build',
  run: function () { return Array.from(arr100).length; },
  expect: 100,
});
bench({
  name: 'guard/array-shift-drain-100', unit: 'drain',
  run: function () { var a = arr100.slice(), s = 0; while (a.length) s += a.shift(); return s; },
  expect: 4950,
});
bench({
  name: 'typedarray/alloc-f32-256', unit: 'alloc', before: keepReset,
  run: function () { keepPush(new Float32Array(256)); return KEEP.length > 0 ? 1 : 0; },
  expect: 1,
});
