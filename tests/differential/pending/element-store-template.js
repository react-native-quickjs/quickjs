// Evaluation and coercion order of `obj[key] = value`, plus every store shape
// the element-store template touches.  Patch 0057 deletes the eleven-dispatch
// parser template at js_parse_assign_expr2() and emits [obj][key][val]
// put_array_el instead, so the *only* thing that keeps the semantics is
// OP_put_array_el's own ordering.  This file is the proof.
//
// Written for patch 0057; see docs/element-store-template.md.
//
// Mode discipline: node runs a corpus file as an ES module (strict), the qjs
// CLI runs it as a script (sloppy).  Anything that depends on the surrounding
// mode is therefore built with `new Function(...)` (always sloppy) or with an
// explicit "use strict" function body, never left to the ambient mode.

var out;
function log(s) { out.push(String(s)); }
function run(name, f) {
  out = [];
  var r;
  try { f(); r = 'ok'; } catch (e) { r = e.constructor.name; }
  print(name + ' | ' + out.join(',') + ' | ' + r);
}

/* ---- 1. the coercion order the template existed to distort ------------- */

// Spec (ES2024 6.2.5.6 PutValue): the RHS is evaluated first, then
// ToObject(base), then ToPropertyKey(key).  So: rhs,key.
run('order-nonnull', function () {
  var o = {};
  var k = { toString: function () { log('key'); return 'k'; } };
  o[k] = (log('rhs'), 1);
  log('v=' + o.k);
});

// The nullish base still throws, and still throws only after the RHS.  The
// *key coercion* on this path is a pre-existing quickjs-ng divergence from
// node (node never coerces the key; quickjs-ng coerces it, then throws), so
// this row deliberately uses a plain key and asserts only the throw.  The
// divergence itself is recorded in
// tests/differential/pending/element-store-nullish-and-compound-order.js.
run('order-null', function () {
  var o = null;
  o['k'] = (log('rhs'), 1);
});

run('order-undefined', function () {
  var o = void 0;
  o['k'] = (log('rhs'), 1);
});

run('order-base-key-rhs', function () {
  var o = {};
  (log('base'), o)[(log('keyexpr'), 'k')] = (log('rhs'), 1);
  log('v=' + o.k);
});

run('key-throws-nonnull', function () {
  var o = {};
  var k = { toString: function () { log('key'); throw new TypeError('boom'); } };
  o[k] = (log('rhs'), 1);
});

run('rhs-throws', function () {
  var o = {};
  var k = { toString: function () { log('key'); return 'k'; } };
  o[k] = (function () { log('rhs'); throw new RangeError('r'); })();
  log('unreachable');
});

run('valueOf-preferred', function () {
  var o = {};
  var k = {
    valueOf: function () { log('valueOf'); return 3; },
    toString: function () { log('toString'); return 't'; }
  };
  o[k] = 1;
  log('keys=' + Object.keys(o).join('/'));
});

/* ---- 2. the key coercion mutating the base ----------------------------- */

run('key-mutates-object', function () {
  var o = { a: 1 };
  var k = { toString: function () { log('key'); o.a = 99; return 'a'; } };
  o[k] = (log('rhs'), 2);
  log('a=' + o.a);
});

run('key-truncates-array', function () {
  var a = [1, 2, 3];
  var k = { valueOf: function () { log('key'); a.length = 0; return 1; } };
  a[k] = (log('rhs'), 9);
  log('len=' + a.length + ' a1=' + a[1] + ' keys=' + Object.keys(a).join('/'));
});

run('key-freezes-object', function () {
  var o = {};
  var k = { toString: function () { log('key'); Object.freeze(o); return 'z'; } };
  o[k] = 1;
  log('z=' + o.z);
});

run('rhs-mutates-base', function () {
  var a = [1, 2, 3];
  a[1] = (a.length = 0, 7);
  log('len=' + a.length + ' keys=' + Object.keys(a).join('/'));
});

/* ---- 3. key shapes ------------------------------------------------------ */

run('symbol-key', function () {
  var o = {};
  var s = Symbol('s');
  o[s] = 5;
  log('v=' + o[s] + ' ownkeys=' + Object.keys(o).length);
});

run('null-base-symbol-key', function () {
  var o = null;
  var s = Symbol('q');
  o[s] = 1;
});

run('negative-zero-key', function () {
  var o = {};
  o[-0] = 1;
  log('objkeys=' + Object.keys(o).join('/'));
  var a = [];
  a[-0] = 5;
  log('a0=' + a[0] + ' len=' + a.length + ' keys=' + Object.keys(a).join('/'));
});

run('negative-index', function () {
  var a = [1, 2, 3];
  a[-1] = 9;
  log('len=' + a.length + ' m1=' + a[-1] + ' keys=' + Object.keys(a).join('/'));
});

run('numeric-string-vs-number', function () {
  var a = [];
  a['0'] = 1; a[1.5] = 2; a['01'] = 3; a[2] = 4;
  log('len=' + a.length + ' keys=' + Object.keys(a).join('/'));
});

run('float-integral-key', function () {
  var a = [0, 0, 0];
  var i = 1.0;
  a[i] = 7;
  a[2.0] = 8;
  log('a=' + a.join('/') + ' len=' + a.length);
});

run('huge-index', function () {
  var a = [];
  a[4294967294] = 1;   // last array index
  log('len=' + a.length);
  var b = [];
  b[4294967295] = 1;   // not an array index
  log('len=' + b.length + ' keys=' + Object.keys(b).join('/'));
});

/* ---- 4. base shapes ----------------------------------------------------- */

run('holey-array-store', function () {
  var a = [1, , 3];
  a[1] = 2;
  a[3] = 4;
  log(a.join('/') + ' len=' + a.length);
});

run('typed-array-oob', function () {
  var t = new Int32Array(2);
  t[5] = 3;
  log('len=' + t.length + ' v5=' + t[5] + ' keys=' + Object.keys(t).join('/'));
});

run('typed-array-key-coercion', function () {
  var t = new Int32Array(2);
  var k = { valueOf: function () { log('key'); return 1; } };
  t[k] = (log('rhs'), 9);
  log('v=' + t[1]);
});

run('typed-array-value-coercion-detaches', function () {
  var t = new Float64Array(2);
  var v = { valueOf: function () { log('valueOf'); return 1.5; } };
  t[0] = v;
  log('v=' + t[0]);
});

run('frozen-sloppy', new Function(
  'var o = Object.freeze({a:1}); o["a"] = 2; return o.a;'
).bind(null));

run('frozen-sloppy-observed', function () {
  var f = new Function('log', 'var o = Object.freeze({a:1}); o["a"] = 2; log("a=" + o.a);');
  f(log);
});

run('frozen-strict', function () {
  'use strict';
  var o = Object.freeze({ a: 1 });
  o['a'] = 2;
  log('a=' + o.a);
});

run('nonwritable-strict', function () {
  'use strict';
  var o = {};
  Object.defineProperty(o, 'x', { value: 1, writable: false });
  o['x'] = 2;
});

run('nonextensible-array-append', function () {
  var a = [1];
  Object.preventExtensions(a);
  a[1] = 2;
  log('len=' + a.length + ' a1=' + a[1]);
});

run('string-base-sloppy', function () {
  var f = new Function('log', 'var s = "abc"; s[0] = "z"; log("s=" + s);');
  f(log);
});

run('number-base-sloppy', function () {
  var f = new Function('log', 'var n = 5; n[0] = 1; log("ok");');
  f(log);
});

run('string-base-strict', function () {
  'use strict';
  var s = 'abc';
  s[0] = 'z';
});

/* ---- 5. accessors and proxies ------------------------------------------ */

run('proto-setter', function () {
  var proto = {};
  Object.defineProperty(proto, 'x', {
    set: function (v) { log('setter:' + v + ':this=' + (this === o)); },
    get: function () { return 42; },
    configurable: true
  });
  var o = Object.create(proto);
  var k = { toString: function () { log('key'); return 'x'; } };
  o[k] = (log('rhs'), 3);
  log('x=' + o.x);
});

run('own-setter', function () {
  var o = {};
  Object.defineProperty(o, 'y', { set: function (v) { log('set:' + v); }, configurable: true });
  o['y'] = 11;
});

run('proxy-set-trap', function () {
  var p = new Proxy({}, {
    set: function (t, k, v) { log('set:' + String(k) + '=' + v); t[k] = v; return true; },
    defineProperty: function (t, k, d) { log('dp:' + String(k)); Object.defineProperty(t, k, d); return true; },
    getOwnPropertyDescriptor: function (t, k) { log('gopd:' + String(k)); return Object.getOwnPropertyDescriptor(t, k); },
    has: function (t, k) { log('has:' + String(k)); return k in t; }
  });
  var k = { toString: function () { log('key'); return 'k'; } };
  p[k] = (log('rhs'), 7);
});

run('proxy-set-returns-false-strict', function () {
  'use strict';
  var p = new Proxy({}, { set: function () { log('set'); return false; } });
  p['a'] = 1;
});

run('proxy-set-returns-false-sloppy', function () {
  var f = new Function('log',
    'var p = new Proxy({}, { set: function(){ log("set"); return false; } }); p["a"] = 1; log("no throw");');
  f(log);
});

run('proxy-in-prototype-chain', function () {
  var p = new Proxy({}, { set: function (t, k, v, r) { log('protoset:' + String(k)); return true; } });
  var o = Object.create(p);
  o['w'] = 1;
  log('own=' + Object.prototype.hasOwnProperty.call(o, 'w'));
});

/* ---- 6. compound and logical forms share get_lvalue/put_lvalue --------- */

// The compound forms go through get_lvalue(keep = true), which patch 0057 does
// not touch.  How many times they coerce the key is a pre-existing divergence
// (quickjs-ng once, node twice — once for the Get and once for the Set), so
// these rows assert the *result*, not the coercion trace.  The divergence is
// recorded in
// tests/differential/pending/element-store-nullish-and-compound-order.js.
run('compound-add', function () {
  var o = { k: 1 };
  var k = { toString: function () { return 'k'; } };
  o[k] += (log('rhs'), 2);
  log('k=' + o.k);
});

run('increment', function () {
  var o = { k: 1 };
  var k = { toString: function () { return 'k'; } };
  o[k]++;
  log('k=' + o.k);
});

run('decrement-prefix', function () {
  var a = [5];
  --a[0];
  log('a0=' + a[0]);
});

run('logical-nullish-assign', function () {
  var o = {};
  var k = { toString: function () { return 'k'; } };
  o[k] ??= (log('rhs'), 1);
  log('k=' + o.k);
  o[k] ??= (log('rhs2'), 2);
  log('k=' + o.k);
});

run('logical-or-assign', function () {
  var o = { k: 0 };
  o['k'] ||= 5;
  log('k=' + o.k);
});

run('exponent-assign', function () {
  var o = { k: 2 };
  o['k'] **= 3;
  log('k=' + o.k);
});

/* ---- 7. destructuring and for-of/for-in targets ------------------------ */

run('destructuring-element-target', function () {
  var o = {};
  var k = { toString: function () { log('key'); return 'a'; } };
  var r = [o[k]] = [1];
  log('a=' + o.a + ' r=' + JSON.stringify(r));
});

run('for-of-element-target', function () {
  var o = {};
  for (o['x'] of [1, 2, 3]) { /* nothing */ }
  log('x=' + o.x);
});

run('for-in-element-target', function () {
  var o = {};
  var src = { p: 1, q: 2 };
  for (o['k'] in src) { /* nothing */ }
  log('k=' + o.k);
});

/* ---- 8. the assignment's own value, and nesting ------------------------ */

run('assignment-value', function () {
  var o = {};
  var v = (o['a'] = 5);
  log('v=' + v + ' a=' + o.a);
});

run('nested-stores', function () {
  var o = {};
  var k1 = { toString: function () { log('k1'); return 'a'; } };
  var k2 = { toString: function () { log('k2'); return 'b'; } };
  o[k1] = (o[k2] = (log('rhs'), 1));
  log('a=' + o.a + ' b=' + o.b);
});

run('chained', function () {
  var a = {}, b = {};
  a['x'] = b['y'] = 3;
  log('x=' + a.x + ' y=' + b.y);
});

run('store-in-key-of-outer', function () {
  var o = {};
  o[(o['inner'] = 'k')] = 1;
  log('inner=' + o.inner + ' k=' + o.k);
});

/* ---- 9. arguments-elided sites (patch 0048) ---------------------------- */

run('arguments-store-unmapped', function () {
  var f = new Function('log',
    '"use strict"; return function(a){ arguments[0] = 5; log("arg0=" + arguments[0] + " a=" + a); };');
  f(log)(1);
});

run('arguments-store-mapped', function () {
  var f = new Function('log',
    'return function(a){ arguments[0] = 5; log("arg0=" + arguments[0] + " a=" + a); };');
  f(log)(1);
});

run('arguments-elided-index', function () {
  var f = new Function('log', 'return function(){ return arguments[0]; };');
  log('r=' + f(log)(42));
});

/* ---- 10. loops, so the emitted form is exercised many times ------------ */

run('loop-dense-store', function () {
  var a = [];
  for (var i = 0; i < 8; i++) a[i] = i * i;
  log(a.join('/'));
});

run('loop-object-store', function () {
  var o = {};
  var keys = ['a', 'b', 'c'];
  for (var i = 0; i < keys.length; i++) o[keys[i]] = i;
  log(JSON.stringify(o));
});

run('loop-2d-store', function () {
  var m = [[0, 0], [0, 0]];
  for (var i = 0; i < 2; i++) for (var j = 0; j < 2; j++) m[i][j] = i * 2 + j;
  log(JSON.stringify(m));
});
