// Differential corpus for the for-in shape-pinning patch.
// Runs on node and on qjs; output must be byte-identical.
var out = [];
function log(name, v) { out.push(name + ': ' + v); }
function keys(o) { var a = []; for (var k in o) a.push(k); return a.join(','); }
function T(name, fn) {
  var r;
  try { r = fn(); } catch (e) { r = 'THROW ' + (e && e.name); }
  log(name, r);
}

// ---------- 1. basic shapes and ordering ----------
T('plain', function () { return keys({ a: 1, b: 2, c: 3 }); });
T('empty', function () { return keys({}); });
T('null-proto', function () { var o = Object.create(null); o.z = 1; o.a = 2; return keys(o); });
T('int-keys', function () { var o = {}; o.b = 1; o[2] = 2; o.a = 3; o[1] = 4; o[0] = 5; return keys(o); });
T('int-keys-lit', function () { return keys({ 10: 1, b: 2, 2: 3, a: 4 }); });
T('big-int-keys', function () { var o = {}; o.x = 1; o['4294967294'] = 2; o['4294967295'] = 3; o['2147483648'] = 4; return keys(o); });
T('neg-and-float', function () { var o = {}; o['-1'] = 1; o['1.5'] = 2; o['01'] = 3; o['1'] = 4; o.a = 5; return keys(o); });
T('sym-mixed', function () { var o = { a: 1 }; o[Symbol('s')] = 2; o.b = 3; return keys(o); });
T('nonenum-own', function () {
  var o = { a: 1 }; Object.defineProperty(o, 'h', { value: 2, enumerable: false }); o.b = 3; return keys(o);
});
T('getter', function () { var o = { get g() { return 1; }, b: 2 }; return keys(o); });

// ---------- 2. prototype chain ----------
T('proto-enum', function () { var p = { pa: 1, pb: 2 }; var o = Object.create(p); o.a = 3; return keys(o); });
T('proto-shadow', function () { var p = { a: 1, b: 2 }; var o = Object.create(p); o.a = 9; return keys(o); });
T('proto-nonenum-shadow', function () {
  var p = { a: 1, b: 2 }; var o = Object.create(p);
  Object.defineProperty(o, 'a', { value: 9, enumerable: false });
  return keys(o);
});
T('proto-deep', function () {
  var a = { x: 1 }; var b = Object.create(a); b.y = 2; var c = Object.create(b); c.z = 3; return keys(c);
});
T('objproto-extended', function () {
  Object.prototype.__diffProbe = 7;
  var r = keys({ a: 1, b: 2 });
  delete Object.prototype.__diffProbe;
  return r;
});
T('objproto-nonenum', function () {
  Object.defineProperty(Object.prototype, '__diffProbe2', { value: 7, enumerable: false, configurable: true });
  var r = keys({ a: 1, b: 2 });
  delete Object.prototype.__diffProbe2;
  return r;
});

// ---------- 3. mutation during iteration ----------
T('add-during', function () {
  var o = { a: 1, b: 2 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') o.c = 3; }
  return r.join(',') + ' | ' + keys(o);
});
T('delete-current', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); delete o[k]; }
  return r.join(',') + ' | ' + keys(o);
});
T('delete-next', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') delete o.b; }
  return r.join(',');
});
T('delete-prev', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'b') delete o.a; }
  return r.join(',');
});
T('delete-then-readd', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') { delete o.c; o.c = 9; } }
  return r.join(',');
});
T('delete-all-then-readd-same', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') { delete o.b; delete o.c; o.b = 1; o.c = 2; } }
  return r.join(',');
});
T('reconfig-nonenum-during', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.defineProperty(o, 'c', { enumerable: false }); }
  return r.join(',');
});
T('reconfig-enum-during', function () {
  var o = { a: 1 };
  Object.defineProperty(o, 'b', { value: 2, enumerable: false, configurable: true });
  o.c = 3;
  var r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.defineProperty(o, 'b', { enumerable: true }); }
  return r.join(',');
});
T('proto-swap-during', function () {
  var o = { a: 1, b: 2 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.setPrototypeOf(o, { p: 1 }); }
  return r.join(',');
});
T('proto-null-during', function () {
  var o = { a: 1, b: 2 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.setPrototypeOf(o, null); }
  return r.join(',');
});
T('freeze-during', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.freeze(o); }
  return r.join(',');
});
T('seal-during', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') Object.seal(o); }
  return r.join(',');
});
T('add-int-key-during', function () {
  var o = { a: 1, b: 2 }, r = [];
  for (var k in o) { r.push(k); if (k === 'a') o[0] = 9; }
  return r.join(',');
});
T('many-delete-compact', function () {
  var o = {}, i;
  for (i = 0; i < 24; i++) o['k' + i] = i;
  var r = [];
  for (var k in o) { r.push(k); if (r.length === 2) { for (i = 2; i < 20; i++) delete o['k' + i]; } }
  return r.join(',');
});
T('getter-mutates', function () {
  var o = { a: 1 };
  Object.defineProperty(o, 'g', { enumerable: true, get: function () { o.late = 1; return 5; } });
  o.b = 2;
  var r = [];
  for (var k in o) r.push(k + '=' + o[k]);
  return r.join(',');
});
T('proto-getter-mutates', function () {
  var p = {};
  Object.defineProperty(p, 'pg', { enumerable: true, get: function () { return 1; } });
  var o = Object.create(p); o.a = 1; o.b = 2;
  var r = [];
  for (var k in o) { r.push(k); if (k === 'a') delete o.b; }
  return r.join(',');
});

// ---------- 4. exotic receivers ----------
T('array', function () { return keys([1, 2, 3]); });
T('array-holes', function () { var a = [1, , 3]; a[10] = 4; return keys(a); });
T('array-extra-props', function () { var a = [1, 2]; a.foo = 3; a[5] = 4; return keys(a); });
T('arguments', function () { return (function () { return keys(arguments); })(1, 2, 3); });
T('string-wrapper', function () { var s = new String('abc'); s.x = 1; return keys(s); });
T('string-primitive', function () { return keys('abc'); });
T('number-primitive', function () { return keys(42); });
T('bool-primitive', function () { return keys(true); });
T('typed-array', function () { var t = new Uint8Array(3); t.foo = 1; return keys(t); });
T('function-obj', function () { function f() {} f.a = 1; return keys(f); });
T('date', function () { var d = new Date(0); d.x = 1; return keys(d); });
T('map-obj', function () { var m = new Map(); m.x = 1; return keys(m); });
T('regexp', function () { var r = /a/g; r.x = 1; return keys(r); });
T('error-obj', function () { var e = new Error('x'); e.custom = 1; return keys(e); });
T('null-undef', function () { return keys(null) + '|' + keys(undefined); });
T('proxy-ownKeys', function () {
  var p = new Proxy({ a: 1, b: 2 }, {
    ownKeys: function (t) { return ['b', 'a', 'c']; },
    getOwnPropertyDescriptor: function (t, k) {
      if (k === 'c') return { value: 3, enumerable: true, configurable: true };
      return Object.getOwnPropertyDescriptor(t, k);
    }
  });
  return keys(p);
});
T('proxy-as-proto', function () {
  var p = new Proxy({ pa: 1 }, {});
  var o = Object.create(p); o.a = 1; o.b = 2;
  var r = [];
  for (var k in o) { r.push(k); if (k === 'a') delete o.b; }
  return r.join(',');
});
T('proxy-hasprop-proto-mutates', function () {
  var hits = 0;
  var p = new Proxy({}, { has: function (t, k) { hits++; return false; } });
  var o = Object.create(p); o.a = 1; o.b = 2; o.c = 3;
  var r = [];
  for (var k in o) { r.push(k); if (k === 'a') delete o.b; }
  return r.join(',');
});

// ---------- 5. structure / control flow ----------
T('nested-same-object', function () {
  var o = { a: 1, b: 2 }, r = [];
  for (var i in o) for (var j in o) r.push(i + j);
  return r.join(',');
});
T('nested-mutate-inner', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var i in o) { for (var j in o) { r.push(i + j); if (i === 'a' && j === 'a') delete o.b; } }
  return r.join(',');
});
T('break-resume', function () {
  var o = { a: 1, b: 2, c: 3 }, r = [];
  for (var k in o) { if (k === 'b') break; r.push(k); }
  for (var k2 in o) r.push(k2);
  return r.join(',');
});
T('for-in-destructure', function () {
  var o = { a: 1, b: 2 }, r = [];
  var t = {};
  for (t.k in o) r.push(t.k);
  return r.join(',');
});
T('lots-of-keys', function () {
  var o = {}, i;
  for (i = 0; i < 40; i++) o['p' + i] = i;
  return keys(o).length + ':' + keys(o).slice(0, 20);
});
T('shared-shape-two-objects', function () {
  function mk(x) { return { a: x, b: x + 1, c: x + 2 }; }
  var o1 = mk(1), o2 = mk(2), r = [];
  for (var k in o1) { r.push(k); if (k === 'a') delete o2.b; }
  for (var k2 in o2) r.push(k2);
  return r.join(',');
});
// These four target the "unhashed shape" hazard specifically: after a delete or
// a reconfigure the object owns a private, unhashed shape which the engine
// mutates IN PLACE (and may realloc) regardless of how many references exist.
// Anything that caches a pointer to such a shape reads freed or grown memory.
T('unhashed-then-grow-during', function () {
  var o = { a: 1, b: 2, c: 3, d: 4 };
  delete o.b;                       // o now owns a private, unhashed shape
  var r = [];
  for (var k in o) {
    r.push(k);
    if (r.length === 1) { o.e = 5; o.f = 6; o.g = 7; o.h = 8; o.i = 9; o.j = 10; }
  }
  return r.join(',');
});
T('unhashed-then-delete-during', function () {
  var o = { a: 1, b: 2, c: 3, d: 4 };
  delete o.b;
  var r = [];
  for (var k in o) { r.push(k); if (r.length === 1) delete o.d; }
  return r.join(',');
});
T('reconfigured-then-grow-during', function () {
  var o = { a: 1, b: 2, c: 3 };
  Object.defineProperty(o, 'b', { enumerable: false });  // unhashed shape
  var r = [];
  for (var k in o) { r.push(k); if (r.length === 1) { o.d = 4; o.e = 5; o.f = 6; } }
  return r.join(',');
});
T('grow-in-place-during', function () {
  // a fresh object's shape is hashed but uniquely owned; growing it must not be
  // observable through an in-flight enumeration
  var o = {}; o.a = 1; o.b = 2;
  var r = [];
  for (var k in o) { r.push(k); if (r.length === 1) { o.c = 3; o.d = 4; o.e = 5; } }
  return r.join(',');
});

T('json-parsed', function () { return keys(JSON.parse('{"b":1,"a":2,"3":4}')); });
T('object-assign', function () { return keys(Object.assign({}, { x: 1 }, { y: 2 })); });
T('spread', function () { var o = { a: 1, b: 2 }; var c = Object.assign({}, o); c.z = 3; return keys(c); });
T('class-instance', function () {
  function C() { this.a = 1; this.b = 2; }
  C.prototype.m = function () {};
  return keys(new C());
});
T('global-like', function () { var o = {}; o['constructor'] = 1; o['toString'] = 2; return keys(o); });
T('keys-vs-forin', function () {
  var o = { b: 1, 2: 2, a: 3, 0: 4 };
  return Object.keys(o).join(',') + ' | ' + keys(o);
});

print(out.join('\n'));
