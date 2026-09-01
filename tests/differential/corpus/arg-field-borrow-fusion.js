/* Adversarial corpus for OP_get_arg_field_nr (patch 0056).

   The opcode fuses `get_arg(n) get_field(atom)` -- `someParam.someField` -- into
   one instruction that reads the receiver STRAIGHT OUT OF arg_buf[n] and never
   pushes it to the operand stack.  Because it is never pushed, it is never
   js_dup'd and never JS_FreeValue'd: the fused handler BORROWS it.

   ## COVERAGE FIRST

   Patch 0020 shipped a corpus that exercised its optimization zero times,
   because a top-level `var` in a script is a global and compiles to OP_get_var.
   Everything here therefore runs inside a function, and the shape that fires the
   peephole is specifically

       function f(receiverParam) { return receiverParam.field; }

   `receiverParam` must be a PARAMETER, the access must be a plain field read
   whose receiver is consumed (not `p.m()`, which emits OP_get_field2 and keeps
   the receiver), and the field must not be `length` (that becomes
   OP_get_length).  Coverage was verified with a -DQJS_R13_STATS build, which
   counts both emitted sites and executed hits.

   ## The case this file exists for: the borrow

   A borrowed receiver is only safe if NOTHING between the load and the use can
   drop the last reference to it.  For an argument register there are three ways
   user code can reach arg_buf[n] while the fused instruction is mid-flight, and
   all three run through a getter or a Proxy trap the field read itself invokes:

     (a) a closure that captured the parameter.  get_var_ref() points a JSVarRef
         straight at &sf->arg_buf[idx] (quickjs.c ~21060), exactly as a captured
         local's points at &sf->var_buf[idx], so `p = null` inside the getter
         frees the object the fused handler is reading through.
     (b) a MAPPED `arguments` object in sloppy mode.  `arguments[0] = null`
         writes through the same JSVarRef machinery into arg_buf[0].
     (c) a Proxy `get` trap, which is the same as (a) and (b) but arrives
         through JS_GetPropertyInternal's exotic path rather than the IC.

   Cases 4, 5, 6 and 7 below are those three, plus the same shapes with a GC
   forced inside the trap so that a freed receiver is actually reused rather
   than merely unreferenced.

   ## The rest

     case  targets
     ----  ----------------------------------------------------------------
     1     the plain fused read at arg indices 0..3 and >3 (general OP_get_arg)
     2     prototype-chain reads and the proto IC entry
     3     an accessor on the receiver, and one on its prototype
     4     the parameter reassigned by the field's own getter        (borrow)
     5     the parameter reassigned through a captured-variable closure (borrow)
     6     the parameter reassigned through a MAPPED arguments object   (borrow)
     7     a Proxy argument whose get trap mutates the caller's frame   (borrow)
     8     a GC forced from inside the getter, with the only other
           reference to the receiver dropped first                    (borrow)
     9     `.length` on a parameter -- must NOT fuse (OP_get_length)
     10    `param.m()` -- must NOT fuse (OP_get_field2 keeps the receiver)
     11    non-object arguments: undefined, null, number, string, boolean,
           symbol -- the TypeError text and the primitive-wrapper reads
     12    a missing argument (argc < arg_count, arg_buf padded with undefined)
     13    delete / redefine between reads, so the IC must invalidate
     14    the parameter reassigned by ordinary code, then read again
     15    the interaction with patch 0048's `arguments` reification elision:
           the same function both fuses an argument field read and touches
           `arguments`, in the elided and non-elided shapes
     16    exotic receivers reached through a parameter: a Proxy without a get
           trap, a typed array, a String object, and a frozen object
     17    a megamorphic site: many shapes through one fused instruction
*/

function say(label, v) {
  var t = typeof v;
  if (t === 'symbol') { print(label + ': symbol'); return; }
  if (v === null) { print(label + ': null'); return; }
  if (t === 'object' || t === 'function') { print(label + ': [' + t + ']'); return; }
  print(label + ': ' + String(v));
}

/* The wording of the "read a property of undefined" TypeError is engine
   private -- quickjs says "cannot read property 'v' of undefined", node says
   "Cannot read properties of undefined (reading 'v')" -- so this normalises to
   the error's NAME plus whether the message names the offending key, which is
   the part the specification and this optimization actually constrain. */
function tryIt(label, fn) {
  try {
    say(label, fn());
  } catch (e) {
    if (e instanceof TypeError) {
      print(label + ': TypeError names-key=' + (e.message.indexOf("'v'") >= 0));
    } else {
      print(label + ': ' + e.name + ': ' + e.message);
    }
  }
}

/* ------------------------------------------------------------------ case 1 */
/* Plain fused reads at every argument index that has a short opcode, plus one
   past them so the general 3-byte OP_get_arg is exercised too. */
function readA0(a) { return a.v; }
function readA1(a, b) { return b.v; }
function readA2(a, b, c) { return c.v; }
function readA3(a, b, c, d) { return d.v; }
function readA5(a, b, c, d, e, f) { return f.v; }

print('--- case 1');
say('a0', readA0({ v: 10 }));
say('a1', readA1(0, { v: 11 }));
say('a2', readA2(0, 0, { v: 12 }));
say('a3', readA3(0, 0, 0, { v: 13 }));
say('a5', readA5(0, 0, 0, 0, 0, { v: 15 }));

/* Warm the sites so the inline cache is filled and the IC path, not the
   find_own_property walk, is what the later cases perturb. */
function warm(fn, arg, n) {
  var last;
  for (var i = 0; i < n; i++) last = fn(arg);
  return last;
}
say('warm own', warm(readA0, { v: 1 }, 50));

/* ------------------------------------------------------------------ case 2 */
print('--- case 2');
function Base() {}
Base.prototype.v = 'from-proto';
Base.prototype.w = 'proto-w';
function Derived() { this.own = 1; }
Derived.prototype = new Base();
say('proto', warm(readA0, new Derived(), 50));
function readW(a) { return a.w; }
say('proto deep', warm(readW, new Derived(), 50));
/* An own property shadowing the prototype one, at the same site. */
var shadow = new Derived();
shadow.v = 'own-wins';
say('shadowed', readA0(shadow));
say('proto again', readA0(new Derived()));

/* ------------------------------------------------------------------ case 3 */
print('--- case 3');
var withGetter = {};
Object.defineProperty(withGetter, 'v', { get: function () { return 'getter'; }, configurable: true });
say('own accessor', warm(readA0, withGetter, 20));
function ProtoAcc() {}
Object.defineProperty(ProtoAcc.prototype, 'v', { get: function () { return 'proto-getter'; } });
say('proto accessor', warm(readA0, new ProtoAcc(), 20));

/* ------------------------------------------------------------- cases 4-8   */
/* THE BORROW CASES.  Each one drops the caller's last reference to the
   receiver from inside the very field read that is borrowing it. */

print('--- case 4');
/* The getter reassigns the parameter it was reached through.  `a` is captured
   by the getter's closure, so the write lands in arg_buf[0]. */
function reassignByGetter(a) {
  return a.v;
}
function makeSelfClobbering(container) {
  var o = {};
  Object.defineProperty(o, 'v', {
    get: function () {
      container.slot = null;      /* drop the outside reference */
      return 'survived';
    }
  });
  return o;
}
var box4 = { slot: makeSelfClobbering(null) };
box4.slot = makeSelfClobbering(box4);
say('self-clobber', reassignByGetter(box4.slot));

print('--- case 5');
/* The parameter itself is the captured variable, and the getter nulls it. */
function capturedParamClobber(p) {
  var o = {};
  Object.defineProperty(o, 'v', {
    get: function () {
      p = null;                   /* writes through the JSVarRef into arg_buf[0] */
      return 'ok';
    }
  });
  p = o;                          /* the only reference to o is now arg_buf[0] */
  return p.v;                     /* fused: borrows arg_buf[0] */
}
say('captured param', capturedParamClobber(null));

/* The same, but the fused read is on a DIFFERENT parameter than the one the
   getter clobbers, so the site stays monomorphic and warm. */
function capturedParamClobber2(p, q) {
  Object.defineProperty(q, 'v', {
    get: function () {
      p = null;
      q = null;
      return 'ok2';
    },
    configurable: true
  });
  return q.v;
}
say('captured param 2', capturedParamClobber2({}, {}));

print('--- case 6');
/* MAPPED arguments: sloppy mode, simple parameter list, so `arguments[0] = x`
   writes through into arg_buf[0]. */
function mappedClobber(p) {
  var o = {};
  Object.defineProperty(o, 'v', {
    get: function () {
      arguments2[0] = null;       /* the OUTER function's mapped arguments */
      return 'mapped-ok';
    }
  });
  var arguments2 = arguments;
  p = o;
  return p.v;
}
say('mapped arguments', mappedClobber(null));
/* Confirm the mapping really is live in this engine, so the case above is not
   silently a no-op. */
function mappingIsLive(p) {
  arguments[0] = 'changed';
  return p;
}
say('mapping live', mappingIsLive('original'));

print('--- case 7');
/* A Proxy argument whose get trap mutates the caller's frame. */
function proxyClobber(p) {
  var target = { v: 'proxy-v' };
  var prox = new Proxy(target, {
    get: function (t, k, r) {
      p = null;                   /* clobber the borrowed receiver */
      return t[k];
    }
  });
  p = prox;
  return p.v;
}
say('proxy trap clobbers frame', proxyClobber(null));

print('--- case 8');
/* Force a GC inside the getter, after the last outside reference is gone, so a
   freed receiver is actually recycled rather than merely unreferenced. */
function churn() {
  var keep = [];
  for (var i = 0; i < 2000; i++) keep.push({ a: i, b: 'x' + i, c: [i, i, i] });
  return keep.length;
}
function gcInGetter(p) {
  var o = {};
  Object.defineProperty(o, 'v', {
    get: function () {
      p = null;
      churn();
      return 'gc-ok';
    }
  });
  p = o;
  return p.v;
}
say('gc in getter', gcInGetter(null));

/* The same, through the prototype-IC arm rather than the own-property arm. */
function GcProto() {}
Object.defineProperty(GcProto.prototype, 'v', {
  get: function () { churn(); return 'gc-proto-ok'; }
});
say('gc in proto getter', readA0(new GcProto()));

/* ------------------------------------------------------------------ case 9 */
print('--- case 9');
function readLength(a) { return a.length; }
say('array length', readLength([1, 2, 3]));
say('string length', readLength('hello'));
say('function length', readLength(function (x, y) { return x + y; }));
say('object length', readLength({ length: 7 }));

/* ----------------------------------------------------------------- case 10 */
print('--- case 10');
function callMethod(a) { return a.m(); }
say('method call', callMethod({ m: function () { return 'm-result'; }, v: 1 }));
function callMethodThis(a) { return a.m(); }
say('method this', callMethodThis({
  v: 'this-v',
  m: function () { return this.v; }
}));

/* ----------------------------------------------------------------- case 11 */
print('--- case 11');
tryIt('undefined recv', function () { return readA0(undefined); });
tryIt('null recv', function () { return readA0(null); });
say('number recv', readA0(5));
say('string recv', readA0('abc'));
say('boolean recv', readA0(true));
say('symbol recv', readA0(Symbol('s')));
function readToString(a) { return a.toString; }
say('number proto read', typeof readToString(5));
say('string proto read', typeof readToString('abc'));

/* ----------------------------------------------------------------- case 12 */
print('--- case 12');
function missingArg(a, b) {
  if (b === undefined) return 'b-undefined';
  return b.v;
}
say('missing arg', missingArg({ v: 1 }));
say('present arg', missingArg({ v: 1 }, { v: 'present' }));
tryIt('read missing', function () { return readA1({ v: 1 }); });

/* ----------------------------------------------------------------- case 13 */
print('--- case 13');
var mutable = { v: 'first' };
say('before delete', warm(readA0, mutable, 20));
delete mutable.v;
say('after delete', readA0(mutable));
mutable.other = 1;
mutable.v = 'readded';
say('after readd', readA0(mutable));
Object.defineProperty(mutable, 'v', {
  get: function () { return 'now-accessor'; },
  configurable: true
});
say('after redefine as accessor', readA0(mutable));
Object.defineProperty(mutable, 'v', { value: 'now-data', configurable: true, writable: false });
say('after redefine as data', readA0(mutable));

/* A setter appearing on the prototype after the site is warm. */
function Late() { this.k = 1; }
var lateInstance = new Late();
say('late before', warm(readA0, lateInstance, 20));
Object.defineProperty(Late.prototype, 'v', { get: function () { return 'late-proto'; }, configurable: true });
say('late after', readA0(lateInstance));

/* ----------------------------------------------------------------- case 14 */
print('--- case 14');
function reassignThenRead(a) {
  var first = a.v;
  a = { v: 'second' };
  var second = a.v;
  return first + '|' + second;
}
say('reassign then read', reassignThenRead({ v: 'first' }));

/* ----------------------------------------------------------------- case 15 */
print('--- case 15');
/* Patch 0048 elides the reification of `arguments` when every use is
   `arguments[i]` / `arguments.length` / `f.apply(this, arguments)`.  These
   functions do BOTH that and a fused argument field read, which is a
   combination neither patch's own corpus covers. */
function elidedAndFused(a, b) {
  return a.v + ':' + arguments.length + ':' + arguments[1].v;
}
say('elided + fused', elidedAndFused({ v: 'A' }, { v: 'B' }));

function applyAndFused(a, b) {
  function inner(x, y) { return x.v + '/' + y.v; }
  return a.v + '=' + inner.apply(null, arguments);
}
say('apply + fused', applyAndFused({ v: 'P' }, { v: 'Q' }));

/* A REAL arguments object (escapes, so 0048 cannot elide) alongside a fused
   read of the same parameter. */
function realArguments(a) {
  var esc = arguments;
  return a.v + '#' + esc[0].v + '#' + (esc[0] === a);
}
say('real arguments + fused', realArguments({ v: 'R' }));

/* Mutating through the real arguments object, then reading the parameter with
   the fused opcode. */
function mutateThroughArguments(a) {
  var esc = arguments;
  esc[0] = { v: 'mutated' };
  return a.v;
}
say('mutate via arguments', mutateThroughArguments({ v: 'original' }));

/* Strict mode: arguments is UNMAPPED, so the same write must not be visible. */
function strictArguments(a) {
  'use strict';
  var esc = arguments;
  esc[0] = { v: 'mutated' };
  return a.v;
}
say('strict unmapped', strictArguments({ v: 'original' }));

/* ----------------------------------------------------------------- case 16 */
print('--- case 16');
say('proxy no trap', readA0(new Proxy({ v: 'plain-proxy' }, {})));
say('typed array', readA0(new Uint8Array([1, 2, 3])));
say('typed array byteLength', (function (a) { return a.byteLength; })(new Uint8Array([1, 2, 3])));
var strObj = new String('str');
strObj.v = 'boxed';
say('String object', readA0(strObj));
say('frozen', readA0(Object.freeze({ v: 'frozen-v' })));
say('null-proto', readA0(Object.assign(Object.create(null), { v: 'np' })));
tryIt('null-proto missing', function () { return readW(Object.create(null)); });
/* A getter that throws, reached through a parameter. */
tryIt('throwing getter', function () {
  var o = {};
  Object.defineProperty(o, 'v', { get: function () { throw new RangeError('boom'); } });
  return readA0(o);
});

/* ----------------------------------------------------------------- case 17 */
print('--- case 17');
function mega(a) { return a.v; }
var shapes = [];
for (var i = 0; i < 12; i++) {
  var o = {};
  for (var j = 0; j < i; j++) o['pad' + j] = j;
  o.v = 'shape' + i;
  shapes.push(o);
}
var acc = '';
for (var r = 0; r < 3; r++) {
  for (var i = 0; i < shapes.length; i++) acc += mega(shapes[i]) + ',';
}
print('mega: ' + acc);

print('done');
