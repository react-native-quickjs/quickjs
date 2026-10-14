// Guards the register VM named-read cache: the extra ways of a read site
// (three or more receiver shapes), the entry for a holder two prototypes up,
// promotion of the dominant shape, and Array and rope-string receivers. Each block runs its site enough times to fill the cache and then
// changes something a cached entry must notice.

function show(name, v) { print(name + ': ' + v); }

// --- one site, many own-property shapes --------------------------------------
function readX(o) { return o.x; }
(function () {
  var objs = [
    { x: 1 }, { a: 0, x: 2 }, { a: 0, b: 0, x: 3 }, { x: 4, y: 0 },
    { a: 0, b: 0, c: 0, x: 5 }, { q: 0, x: 6 }, { r: 0, s: 0, x: 7 }
  ];
  var s = 0, i, n;
  for (n = 2; n <= objs.length; n++) {
    for (i = 0; i < 400; i++) s += readX(objs[i % n]) * (n + 1);
  }
  show('poly-own', s);
  // a shape that takes over the site, after the cache holds others
  var late = { z: 0, x: 100 };
  for (i = 0; i < 400; i++) s += readX(i % 50 === 0 ? objs[i % 3] : late);
  show('poly-dominant', s);
  // a cached shape changes under the site: add, overwrite, delete, accessor
  var m = { a: 0, x: 2 };
  for (i = 0; i < 60; i++) s += readX(m);
  m.extra = 1;
  for (i = 0; i < 60; i++) s += readX(m);
  delete m.x;
  show('poly-deleted', readX(m) + ' ' + s);
  Object.defineProperty(m, 'x', { get: function () { return 'getter'; }, configurable: true });
  show('poly-getter', readX(m) + ' ' + readX(objs[0]) + ' ' + readX(objs[1]));
  show('poly-nonobject', readX(7) + ' ' + readX('str') + ' ' + readX(true));
})();

// --- one site, many classes, method on the direct prototype -------------------
function A1() { this.v = 1; } A1.prototype.tag = function () { return 'a' + this.v; };
function A2() { this.w = 0; this.v = 2; } A2.prototype.tag = function () { return 'b' + this.v; };
function A3() { this.v = 3; this.w = 0; } A3.prototype.tag = function () { return 'c' + this.v; };
function A4() { this.u = 0; this.v = 4; } A4.prototype.tag = function () { return 'd' + this.v; };
function A5() { this.v = 5; this.u = 0; } A5.prototype.tag = function () { return 'e' + this.v; };
function A6() { this.t = 0; this.v = 6; } A6.prototype.tag = function () { return 'f' + this.v; };
function callTag(o) { return o.tag(); }
function readTag(o) { return o.tag; }
(function () {
  var all = [new A1(), new A2(), new A3(), new A4(), new A5(), new A6()];
  var out = '', i, n;
  for (n = 1; n <= all.length; n++) {
    for (i = 0; i < 300; i++) out = callTag(all[i % n]);
    out += '|';
    for (i = 0; i < n; i++) out += callTag(all[i]);
    show('poly-proto-' + n, out);
  }
  A2.prototype.tag = function () { return 'B' + this.v; };          // overwrite on a holder
  A3.prototype = { tag: function () { return 'C'; } };              // old instances keep the old prototype
  all[3].tag = function () { return 'own-d'; };                     // shadow on the instance
  delete A5.prototype.tag;                                          // falls through to Object.prototype
  Object.prototype.tag = function () { return 'obj'; };
  out = '';
  for (i = 0; i < all.length; i++) out += callTag(all[i]) + ',';
  out += callTag(new A3()) + ',' + (typeof readTag(all[4]));
  delete Object.prototype.tag;
  show('poly-proto-changed', out);
})();

// --- a holder two prototypes up ---------------------------------------------
function Base() {}
Base.prototype.kind = function () { return 'base'; };
Base.prototype.level = 0;
function Mid() {}
Mid.prototype = Object.create(Base.prototype);
Mid.prototype.constructor = Mid;
function Leaf1() { this.a = 1; }
Leaf1.prototype = Object.create(Mid.prototype);
function Leaf2() { this.b = 2; this.a = 1; }
Leaf2.prototype = Object.create(Mid.prototype);
function Leaf3() { this.c = 3; }
Leaf3.prototype = Object.create(Mid.prototype);
function kindOf(o) { return o.kind(); }
function levelOf(o) { return o.level; }
(function () {
  var l1 = new Leaf1(), l2 = new Leaf2(), l3 = new Leaf3(), all = [l1, l2, l3], i, out = '', s = 0;
  for (i = 0; i < 600; i++) { out = kindOf(all[i % 3]); s += levelOf(all[i % 3]); }
  show('depth2', out + ' ' + s);
  Base.prototype.kind = function () { return 'base2'; };             // holder's slot changes
  Base.prototype.level = 5;
  for (i = 0; i < 30; i++) { out = kindOf(all[i % 3]); s += levelOf(all[i % 3]); }
  show('depth2-holder-write', out + ' ' + s);
  Mid.prototype.kind = function () { return 'mid'; };                // the middle link starts to shadow
  out = '';
  for (i = 0; i < 6; i++) out += kindOf(all[i % 3]) + ',';
  show('depth2-mid-shadow', out);
  Leaf2.prototype.kind = function () { return 'leaf2'; };            // the direct prototype shadows
  l3.kind = function () { return 'own3'; };                          // the receiver shadows
  out = '';
  for (i = 0; i < 6; i++) out += kindOf(all[i % 3]) + ',';
  show('depth2-near-shadow', out);
  delete Mid.prototype.kind; delete Leaf2.prototype.kind; delete l3.kind;
  out = '';
  for (i = 0; i < 6; i++) out += kindOf(all[i % 3]) + ',';
  show('depth2-unshadow', out);
  Object.setPrototypeOf(Mid.prototype, { kind: function () { return 'swapped'; }, level: 9 });
  out = '';
  for (i = 0; i < 6; i++) out += kindOf(all[i % 3]) + levelOf(all[i % 3]) + ',';
  show('depth2-reparent', out);
  Object.setPrototypeOf(Mid.prototype, Base.prototype);
  Object.defineProperty(Base.prototype, 'level', { get: function () { return 'G' + (this.a || 0); }, configurable: true });
  out = '';
  for (i = 0; i < 6; i++) out += levelOf(all[i % 3]) + ',';
  show('depth2-getter', out);
  delete Base.prototype.level;
  show('depth2-absent', levelOf(l1) + ' ' + levelOf(l2));
  Mid.prototype.filler1 = 1; Mid.prototype.filler2 = 2;              // the middle shape changes, no shadow
  out = '';
  for (i = 0; i < 6; i++) out += kindOf(all[i % 3]) + ',';
  show('depth2-mid-grows', out);
})();

// --- Array receivers ---------------------------------------------------------
function joinOf(a) { return a.join('-'); }
function tagOf(a) { return a.tag; }
function pushTo(a, v) { return a.push(v); }
(function () {
  var a = [1, 2, 3], b = [4, 5], c = [], i, out = '', n = 0;
  for (i = 0; i < 300; i++) out = joinOf(i % 2 ? a : b);
  show('array-method', out + ' ' + joinOf(c));
  for (i = 0; i < 300; i++) n = pushTo(c, i);
  show('array-push', n + ' ' + c.length + ' ' + c[299]);
  a.tag = 'named'; b.tag = 'other';
  for (i = 0; i < 300; i++) out = tagOf(i % 2 ? a : b);
  show('array-own-named', out + ' ' + tagOf(c) + ' ' + tagOf([]));
  a.tag = 'renamed'; delete b.tag;
  show('array-own-changed', tagOf(a) + ' ' + tagOf(b));
  var saved = Array.prototype.join;
  Array.prototype.join = function () { return 'patched' + this.length; };
  out = joinOf(a) + ' ' + joinOf(b);
  Array.prototype.join = saved;
  show('array-proto-patched', out + ' ' + joinOf(a));
  Array.prototype.tag = 'from-proto';
  show('array-proto-added', tagOf(a) + ' ' + tagOf(b) + ' ' + tagOf(c));
  delete Array.prototype.tag;
  show('array-proto-deleted', tagOf(a) + ' ' + tagOf(b));
  a.join = function () { return 'own-join'; };
  show('array-own-method', joinOf(a) + ' ' + joinOf(b));
  function Sub() {}
  Sub.prototype = Object.create(Array.prototype);
  Sub.prototype.join = function () { return 'sub'; };
  var s = new Sub(), re = Object.setPrototypeOf([7, 8], Sub.prototype);
  show('array-subclass', joinOf(s) + ' ' + joinOf(re) + ' ' + joinOf([9]));
  var nul = Object.setPrototypeOf([1], null);
  show('array-null-proto', typeof tagOf(nul));
  var sparse = [1, , 3], frozen = Object.freeze([1, 2]), like = { 0: 'x', length: 1, join: Array.prototype.join };
  show('array-kinds', joinOf(sparse) + ' ' + joinOf(frozen) + ' ' + joinOf(like) + ' ' + joinOf(new Uint8Array([1, 2])));
  function lenOf(x) { return x.length; }
  for (i = 0; i < 100; i++) n = lenOf(i % 2 ? a : 'four');
  show('array-length', lenOf(a) + ' ' + lenOf(c) + ' ' + lenOf(sparse) + ' ' + lenOf(like) + ' ' + n);
})();

// --- rope-string receivers ---------------------------------------------------
(function () {
  var parts = ['alpha', 'beta', 'gamma', 'delta'], i, n = 0, s = '', out = '';
  for (i = 0; i < 300; i++) {
    s = parts[i % 4] + '/' + parts[(i + 1) % 4] + '/' + i;         // a concatenation result
    n += s.length + s.charCodeAt(1) + s.indexOf('/');
  }
  show('rope-reads', n + ' ' + s.toUpperCase() + ' ' + s.substring(2, 8));
  var big = '';
  for (i = 0; i < 200; i++) { big += parts[i % 4]; n += big.length; }
  show('rope-accumulate', n + ' ' + big.length + ' ' + big.charAt(big.length - 1) + ' ' + big.lastIndexOf('alpha'));
  var saved = String.prototype.indexOf;
  String.prototype.indexOf = function () { return -7; };
  out = (parts[0] + parts[1]).indexOf('b') + ' ' + parts[0].indexOf('p');
  String.prototype.indexOf = saved;
  String.prototype.zzRope = function () { return 'rope:' + this.length; };
  out += ' ' + (parts[0] + parts[1]).indexOf('b') + ' ' + (parts[2] + parts[3]).zzRope();
  delete String.prototype.zzRope;
  show('rope-proto', out + ' ' + typeof (parts[0] + parts[1]).zzRope);
  show('rope-index', (parts[0] + parts[1])[5] + ' ' + (parts[0] + parts[1])[99] + ' ' + (parts[0] + parts[1])['length']);
})();

// --- lazily created slots must not be served from a cache ---------------------
(function () {
  var fs = [], i, ok = 0;
  for (i = 0; i < 40; i++) fs.push(new Function('return ' + i));
  function protoOf(f) { return f.prototype; }
  for (i = 0; i < 40; i++) {
    var p = protoOf(fs[i]);
    if (typeof p === 'object' && p.constructor === fs[i]) ok++;
  }
  show('lazy-prototype', ok);
})();
