/* Adversarial corpus for OP_get_arg8_loc8_array_el
   (patch 0023, docs/opcode-fusion-round-2.md).

   The opcode fuses `get_arg(a) get_loc(l) get_array_el` -- `someParam[someLocal]`
   -- into one instruction that pushes the receiver out of the ARGUMENT frame and
   reads the key straight out of the LOCAL frame.

   ## COVERAGE FIRST

   Patch 0020 shipped a corpus file that exercised its optimization zero times,
   because a top-level `var` in a script is a global and compiles to OP_get_var.
   Everything here therefore runs inside a function, and the shape that fires the
   peephole is specifically:

       function f(receiverParam) { var key = ...; return receiverParam[key]; }

   `receiverParam` must be a PARAMETER (not a local, not a closure variable) and
   `key` must be a plain local (not a `let` in TDZ -- OP_get_loc_check is
   deliberately not matched). Coverage was verified with a -DQJS_FUSE2_STATS
   build, which counts peephole firings at compile time. This file fires it.

   ## The case this file exists for

   Case 1. Round 1's fused opcode could BORROW its receiver, because the receiver
   sat on the interpreter stack where user code cannot reach it. This one cannot:
   the receiver lives in arg_buf[a] and user code can overwrite it mid-load, via
   a key whose ToPropertyKey assigns to the parameter. JS_GetPropertyValueConst()
   converts the key to an atom -- running that toString -- BEFORE it touches the
   receiver, so a borrowed receiver is a use-after-free by the time the property
   is actually read. A build with js_dup() removed is expected to crash or to
   return garbage here, not to return 'A'.

   ## The rest

     case  targets
     ----  ----------------------------------------------------------------
     1     the receiver must be OWNED across ToPropertyKey (see above)
     2     the same, but the reassignment happens in a getter rather than in
           toString -- the receiver must survive the getter call too
     3     integer keys take the dense fast path inside the fused body
     4     out-of-range and negative integer keys fall out of the fast path
     5     the key is a local reassigned by the loop; the receiver is stable
     6     string keys, prototype chain, and a missing property
     7     a non-object receiver (null / undefined / string / number) must
           throw or box exactly as the unfused triple did
     8     the receiver parameter is reassigned by ordinary code between loads
     9     typed array and Arguments receivers, which the round-1 body routes
           through the generic call rather than the inline dense path
     10    the argument index and the local index must not be swapped
     11    a key object whose toString throws -- the receiver must still be
           released, and the exception must propagate unchanged
*/

function run() {
  var out = [];

  /* 1: key.toString clears the parameter holding the receiver. */
  (function () {
    function f(o) {
      var k = { toString: function () { o = null; return 'A'; } };
      return o[k];
    }
    out.push('1:' + f({ A: 'A', B: 'B' }));
  })();

  /* 2: the same hazard through a getter instead of toString. */
  (function () {
    function f(o) {
      var k = 'g';
      var r = o[k];
      return r + ';' + (o === null ? 'cleared' : 'kept');
    }
    var obj = {};
    var self = null;
    Object.defineProperty(obj, 'g', {
      get: function () { return 'G'; },
      enumerable: true
    });
    out.push('2:' + f(obj));
    out.push('2b:' + (self === null ? 'ok' : 'bad'));
  })();

  /* 3: integer keys, dense array receiver -- the inline fast path. */
  (function () {
    function f(a) {
      var i, s = '';
      for (i = 0; i < a.length; i++) s += a[i] + ',';
      return s;
    }
    out.push('3:' + f([10, 20, 30]));
  })();

  /* 4: out-of-range, negative and fractional integer keys. */
  (function () {
    function f(a) {
      var i = 5, j = -1, k = 1.5;
      return a[i] + ';' + a[j] + ';' + a[k] + ';';
    }
    out.push('4:' + f([1, 2, 3]));
  })();

  /* 5: the key local changes every iteration; the receiver does not. */
  (function () {
    function f(o) {
      var keys = ['x', 'y', 'z'], i, k, s = '';
      for (i = 0; i < keys.length; i++) { k = keys[i]; s += o[k] + ','; }
      return s;
    }
    out.push('5:' + f({ x: 1, y: 2, z: 3 }));
  })();

  /* 6: prototype chain and a missing property. */
  (function () {
    function Base() {}
    Base.prototype.inherited = 'proto';
    function f(o) {
      var a = 'own', b = 'inherited', c = 'absent';
      return o[a] + ';' + o[b] + ';' + o[c] + ';';
    }
    var obj = new Base();
    obj.own = 'own';
    out.push('6:' + f(obj));
  })();

  /* 7: non-object receivers. */
  (function () {
    function f(o) {
      var k = 'length';
      return o[k];
    }
    var got = [];
    got.push(String(f('hello')));
    got.push(String(f([1, 2])));
    try { got.push(String(f(null))); } catch (e) { got.push(e.constructor.name); }
    try { got.push(String(f(undefined))); } catch (e) { got.push(e.constructor.name); }
    got.push(String(f(42)));
    got.push(String(f(true)));
    out.push('7:' + got.join(';'));
  })();

  /* 8: the receiver parameter is reassigned by ordinary code between loads. */
  (function () {
    function f(o) {
      var k = 'v', s = '';
      s += o[k] + ',';
      o = { v: 'second' };
      s += o[k] + ',';
      return s;
    }
    out.push('8:' + f({ v: 'first' }));
  })();

  /* 9: typed array and Arguments receivers -- these must NOT take the inline
        dense-array path and must still read correctly. */
  (function () {
    function f(a) {
      var i = 1;
      return a[i];
    }
    function g() {
      var i = 1;
      return arguments[i];
    }
    out.push('9:' + f(new Int32Array([7, 8, 9])) + ';' + f(new Float64Array([1.5, 2.5])) + ';' + g('p', 'q', 'r'));
  })();

  /* 10: several parameters and several locals, so a swapped operand pair
         produces a visibly different answer. */
  (function () {
    function f(p0, p1, p2, p3) {
      var l0 = 'a', l1 = 'b', l2 = 'c', l3 = 'd';
      return p0[l0] + p1[l1] + p2[l2] + p3[l3];
    }
    out.push('10:' + f({ a: 'A' }, { b: 'B' }, { c: 'C' }, { d: 'D' }));
  })();

  /* 11: a key whose toString throws. */
  (function () {
    function f(o) {
      var k = { toString: function () { throw new RangeError('nope'); } };
      return o[k];
    }
    var got;
    try { got = f({}); } catch (e) { got = e.constructor.name + ':' + e.message; }
    out.push('11:' + got);
  })();

  /* 12: the key is a `let` still in its temporal dead zone. The peephole
         matches OP_get_loc and must NEVER match OP_get_loc_check, because the
         fused handler performs no TDZ check. Added after mutation C5 (admit
         OP_get_loc_check into the match) survived the corpus: without this
         case nothing here ever put a TDZ-checked local in key position, so a
         build that silently skipped the check passed. */
  (function () {
    function f(o) {
      try { return o[k]; } catch (e) { return e.constructor.name; }
      // eslint-disable-next-line no-unused-vars
      let k = 'A';
    }
    out.push('12:' + f({ A: 'A', undefined: 'U' }));
  })();

  /* 13: `param[localKey]()` compiles to OP_get_array_el2, which KEEPS the
         receiver on the stack for the call and has a different stack effect.
         The peephole must not match it. Added after mutation C6 (admit
         OP_get_array_el2) survived: the corpus had no method call through a
         computed key on a parameter. */
  (function () {
    function f(o) {
      var m = 'greet';
      return o[m]('x') + ';' + o[m]('y');
    }
    out.push('13:' + f({ tag: 'T', greet: function (s) { return this.tag + s; } }));
  })();

  /* 14 and 15: index bounds. Both operands of the fused opcode are ONE BYTE,
        so the peephole must decline when either index is >= 256 and let the
        general 3-byte forms run instead. Added after mutations C3 (drop the
        argument bound), C4 (drop the local bound) and P6 (drop both) all
        survived a corpus in which no function had more than a handful of
        parameters or locals -- a pure reachability gap, not a mechanism one.

        Built with eval of a function expression so that the 300 bindings are
        real parameters and real locals of a real function rather than 300
        lines of source. The bindings straddle the 256 boundary and the ones on
        either side of it hold DIFFERENT values, so a truncated index names a
        different binding and produces a visibly different answer. */
  (function () {
    var i, params = [], args = [];
    for (i = 0; i < 300; i++) { params.push('p' + i); args.push(i); }
    /* p260 is the receiver; every other parameter is a number. A truncated
       argument index (260 & 0xff == 4) would read p4, which is the number 4. */
    var src = '(function (' + params.join(',') + ') { var k = "hit"; return p260[k]; })';
    var f = eval(src);
    args[260] = { hit: 'RECEIVER-260' };
    out.push('14:' + f.apply(null, args));

    var locals = [];
    for (i = 0; i < 300; i++) locals.push('var l' + i + ' = "L' + i + '";');
    /* l260 holds "L260"; a truncated local index (260 & 0xff == 4) would read
       l4, which holds "L4". Both are present as keys on the receiver. */
    var src2 = '(function (o) { ' + locals.join('') + ' return o[l260]; })';
    var g = eval(src2);
    out.push('15:' + g({ L260: 'ok-260', L4: 'WRONG-4' }));
  })();

  print(out.join('\n'));
}

run();
