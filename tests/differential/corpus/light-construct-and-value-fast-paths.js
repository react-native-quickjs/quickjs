// Guards these register VM fast paths: light-frame constructors, light
// `f.apply(this, arguments)` forwarders, string and null/undefined equality,
// object truthiness, unary tag tests, typed-array and dense-array element
// access, `length` reads, string-method reads and the 16-slot learned instance
// capacity. Each block runs its site enough times to leave the generic path and
// then changes something the fast path must notice.

function show(name, v) { print(name + ': ' + v); }

// --- constructors in light frames -------------------------------------------
function P(x, y) { this.x = x; this.y = y; }
function RetObj(x) { this.x = x; return { other: x }; }
function RetPrim(x) { this.x = x; return 7; }
function Thrower(x) { this.x = x; if (x === 3) throw new Error('ctor ' + x); }
function Few(a) { this.a = a; }
(function () {
  var s = 0, i, o;
  for (i = 0; i < 200; i++) { o = new P(i, 2); s += o.x + o.y; }
  show('ctor-sum', s);
  show('ctor-proto', Object.getPrototypeOf(new P(1, 2)) === P.prototype);
  show('ctor-instanceof', new P(1, 2) instanceof P);
  for (i = 0; i < 50; i++) o = new RetObj(i);
  show('ctor-ret-object', o.other + ' ' + (o instanceof RetObj) + ' ' + o.x);
  for (i = 0; i < 50; i++) o = new RetPrim(i);
  show('ctor-ret-prim', o.x + ' ' + (o instanceof RetPrim));
  var caught = 0;
  for (i = 0; i < 50; i++) {
    try { o = new Thrower(i % 5); } catch (e) { caught++; }
  }
  show('ctor-throw', caught + ' ' + o.x);
  for (i = 0; i < 50; i++) o = new Few();          // fewer arguments than declared
  show('ctor-short-args', o.a + ' ' + ('a' in o));
  P.prototype = { tag: 'swapped' };
  show('ctor-proto-swap', new P(1, 2).tag);
})();

// a function that only contains `new` on a path it does not take
function maybe(n) { if (n < 0) return new P(n, n); return n + 1; }
(function () {
  var s = 0, i;
  for (i = 0; i < 300; i++) s += maybe(i);
  show('contains-new', s + ' ' + maybe(-2).x);
})();

// --- prototype.js style forwarding ------------------------------------------
var Class = { create: function () { return function () { return this.initialize.apply(this, arguments); }; } };
var V = Class.create();
V.prototype = { initialize: function (x, y, z) { this.x = x; this.y = y; this.z = z; } };
var W = Class.create();
W.prototype = { initialize: function () { this.n = arguments.length; this.last = arguments[arguments.length - 1]; } };
function fwd() { return target.apply(this, arguments); }
function target(a, b) { return a + ':' + b + ':' + arguments.length + ':' + (this && this.who); }
(function () {
  var s = 0, i, v;
  for (i = 0; i < 200; i++) { v = new V(i, 1, 2); s += v.x + v.y + v.z; }
  show('klass-sum', s);
  v = new W(1, 2, 3, 4);
  show('klass-varargs', v.n + ' ' + v.last);
  v = new W();
  show('klass-noargs', v.n + ' ' + v.last);
  for (i = 0; i < 50; i++) v = fwd.call({ who: 'me' }, i, 'b', 'c');
  show('forward', v);
  show('forward-noargs', fwd());
  // replace apply: the callee now sees a real arguments object
  var saved = Function.prototype.apply;
  var seen = [];
  Function.prototype.apply = function (t, args) {
    seen.push(Object.prototype.toString.call(args) + ' ' + args.length + ' ' +
              (typeof args.callee) + ' ' + (args.callee === fwd));
    return saved.call(this, t, args);
  };
  var r = fwd.call({ who: 'x' }, 5, 6);
  Function.prototype.apply = saved;
  show('forward-reified', r + ' | ' + seen.join(','));
})();

// --- equality, truthiness, unary tag tests ----------------------------------
(function () {
  var words = ['BT', 'ET', 'Tf', 'BT', 'q', ''];
  var built = 'B' + 'T';
  var n = 0, i, w, o = { k: 1 }, nul = null, und, vals = [o, nul, und, 0, '', 'a', 1.5, false];
  for (i = 0; i < 300; i++) {
    w = words[i % words.length];
    if (w === 'BT') n += 1;
    if (w !== built) n += 10;
    if (w == 'q') n += 100;
    if (w === '') n += 1000;
  }
  show('string-eq', n);
  var out = [];
  for (i = 0; i < vals.length; i++) {
    var v = vals[i];
    out.push((v == null ? 'N' : 'n') + (v != undefined ? 'D' : 'd') + (v === null ? 'S' : 's') +
             (v ? 'T' : 'f') + (!v ? 'F' : 't') + (typeof v === 'undefined' ? 'U' : 'u') +
             (typeof v === 'function' ? 'C' : 'c') + (v === undefined ? 'X' : 'x'));
  }
  show('tag-tests', out.join(' '));
  var isNull = [];
  for (i = 0; i < vals.length; i++) { var b = vals[i] === null; isNull.push(b); }
  show('is-null-values', isNull.join(','));
  show('obj-eq', (o == o) + ' ' + (o == { k: 1 }) + ' ' + (o != nul) + ' ' + (nul == und) + ' ' + (o == '[object Object]'));
})();

// --- elements ---------------------------------------------------------------
(function () {
  var u8 = new Uint8Array(8), i16 = new Int16Array(4), u32 = new Uint32Array(2);
  var f64 = new Float64Array(2), c8 = new Uint8ClampedArray(3), i;
  for (i = 0; i < 64; i++) u8[i & 7] = u8[(i + 1) & 7] + i;
  show('u8', Array.prototype.join.call(u8, ','));
  i16[0] = 70000; i16[1] = -1; i16[2] = 1.9; i16[3] = '12'; i16[9] = 5;
  show('i16', Array.prototype.join.call(i16, ',') + ' ' + i16[9] + ' ' + i16.length);
  u32[0] = -1; u32[1] = 4294967295;
  show('u32', u32[0] + ' ' + u32[1]);
  f64[0] = 1; f64[1] = 0.5;
  show('f64', f64[0] + ' ' + f64[1]);
  c8[0] = 300; c8[1] = -5; c8[2] = 127;
  show('clamped', Array.prototype.join.call(c8, ','));
  var a = [1, 2, 3], h = [1, , 3], objs = [{}, {}];
  for (i = 0; i < 30; i++) a[i % 3] = a[(i + 1) % 3] + 1;
  a[3] = 'end'; a[6] = 'gap';
  show('array', a.join(',') + ' ' + a.length);
  h[1] = 'filled'; h[0] = 'zero';
  show('holey', h.join(',') + ' ' + (1 in h));
  objs[0] = objs[1]; objs[1] = 'x';
  show('array-objs', (typeof objs[0]) + ' ' + objs[1]);
  var frozen = Object.freeze([1, 2]);
  frozen[0] = 9;
  show('frozen', frozen.join(','));
})();

// --- length and string methods ----------------------------------------------
(function () {
  var s = 'hello world', arr = [1, 2, 3], n = 0, i, holder = { text: s, list: arr };
  for (i = 0; i < 200; i++) {
    n += s.length + arr.length + holder.text.length + holder.list.length;
    n += s.charCodeAt(i % 11) + holder.text.charCodeAt(0);
  }
  show('length-and-charcode', n);
  show('charcode-edges', s.charCodeAt(-1) + ' ' + s.charCodeAt(99) + ' ' + s.charCodeAt(1.9) + ' ' + s.charCodeAt());
  arr.length = 1; arr.push(7, 8, 9, 10);
  show('array-length', arr.length + ' ' + holder.list.length);
  var like = { length: 'custom' };
  show('object-length', like.length);
  // the cached method slot must follow the prototype
  var saved = String.prototype.charCodeAt;
  String.prototype.charCodeAt = function () { return -1; };
  var after = s.charCodeAt(0) + holder.text.charCodeAt(0);
  String.prototype.charCodeAt = saved;
  String.prototype.zzExtra = function () { return 'extra:' + this; };
  show('proto-change', after + ' ' + s.charCodeAt(0) + ' ' + s.zzExtra() + ' ' + s.indexOf('w') + ' ' + s.substring(0, 5));
  delete String.prototype.zzExtra;
  show('proto-delete', typeof s.zzExtra);
  Object.defineProperty(String.prototype, 'zzGet', { get: function () { return 'got ' + this.length; }, configurable: true });
  show('proto-getter', s.zzGet);
  delete String.prototype.zzGet;
})();

// --- more than eight instance properties ------------------------------------
function Big(i) {
  this.a = i; this.b = 1; this.c = 2; this.d = 3; this.e = 4; this.f = 5;
  this.g = 6; this.h = 7; this.i = 8; this.j = 9; this.k = 10; this.l = 11;
}
(function () {
  var s = 0, i, o;
  for (i = 0; i < 100; i++) {
    o = new Big(i);
    o.m = 12; o.n = 13; o.o = 14; o.p = 15; o.q = 16; o.r = 17;   // past sixteen
    s += o.a + o.l + o.r;
  }
  show('big-object', s + ' ' + Object.keys(o).join(''));
})();

// --- frame-visible behaviour of light constructors --------------------------
// A light constructor runs without a recorded frame. These pin the observable
// consequences: new.target, Reflect.construct's target, bound constructors, a
// catchable stack overflow under unbounded recursion, and an Error taken
// inside a constructor.
(function () {
  function NT(x) { this.nt = (new.target === NT); this.undef = (new.target === undefined); }
  var a = new NT(1);
  show('newtarget-ctor', a.nt + ' ' + a.undef);

  function Plain() { return new.target; }
  show('newtarget-plain', String(Plain()));

  function Base(x) { this.x = x; this.viaDerived = (new.target === Derived); }
  function Derived(x) { Base.call(this, x); this.y = 2; }
  var d = Reflect.construct(Base, [5], Derived);
  show('reflect-construct', d.x + ' ' + d.viaDerived + ' ' + (d instanceof Base));

  function Bind(x) { this.x = x; }
  var Bound = Bind.bind(null, 9);
  show('bound-ctor', new Bound().x + ' ' + (new Bound() instanceof Bind));

  // unbounded constructor recursion stays a catchable RangeError
  function Recur(n) { this.n = n; if (n > 0) new Recur(n - 1); }
  var caught = 'none';
  try { Recur(100000); } catch (e) { caught = e.constructor.name + ':' + (e instanceof RangeError); }
  show('ctor-recursion', caught);

  // an Error built inside a light constructor carries its message and a stack
  function WithErr() { var e = new Error('inside'); this.msg = e.message; this.hasStack = typeof e.stack === 'string'; }
  var we = new WithErr();
  show('ctor-error', we.msg + ' ' + we.hasStack);

  // new.target through Reflect.construct with a distinct target
  function Target() { this.tn = (new.target && new.target.name) || 'none'; }
  function Other() {}
  show('newtarget-name', Reflect.construct(Target, [], Other).tn);
})();
