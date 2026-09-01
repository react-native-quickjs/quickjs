/* Adversarial corpus for OP_get_arg8_array_el (patch 0059).

   The opcode fuses `get_arg(a) get_array_el` into one instruction. `obj[k]`
   emits `get_x(obj) get_k(k) get_array_el`, so the ARGUMENT folded in is the
   KEY, not the receiver: the receiver stays on the operand stack and the key is
   read straight out of arg_buf[a] and BORROWED -- no js_dup, no JS_FreeValue.

   Do not confuse this with tests/differential/corpus/arg-element-load-fusion.js,
   which covers OP_get_arg8_loc8_array_el (patch 0023). There the argument is the
   RECEIVER and must be OWNED, because JS_GetPropertyValueConst() runs the key's
   ToPropertyKey -- and therefore arbitrary user code -- before it touches the
   receiver. A key does not need that, and the asymmetry is the single most
   likely thing for a reader to get backwards.

   ## COVERAGE FIRST

   Patch 0020 shipped a corpus file that exercised its optimization zero times,
   because a top-level `var` in a script is a global and compiles to OP_get_var.
   Everything here therefore runs inside a function, and the shape that fires the
   peephole is specifically:

       function f(receiver, keyParam) { return receiver[keyParam]; }

   `keyParam` must be a PARAMETER with index < 256. The receiver may be anything
   whose load is not itself fused into a wider instruction: a parameter works
   (`get_arg get_arg get_array_el` -- only the second one fuses), a local works,
   a global works. Coverage was verified with a -DQJS_R14_STATS build, which
   counts peephole firings at compile time and fast/slow arm executions at run
   time. This file fires it.

   ## THE BORROW IS THE WHOLE RISK

   The borrowed key must stay alive for as long as JS_GetPropertyValueConst()
   reads it. The argument slot it lives in can be overwritten mid-instruction in
   two ways -- ordinary assignment to the parameter from inside the key's own
   valueOf/toString (case 1), and a write through a JSVarRef when the parameter
   is captured by a closure (case 2), because get_var_ref() points a captured
   argument's JSVarRef straight at &sf->arg_buf[idx]. Both are safe only because
   JS_ToPrimitive() takes its own reference (`JS_ToPrimitiveFree(ctx,
   js_dup(val), hint)`), so the conversion holds the key alive independently of
   the frame slot. A build that also freed the borrowed key -- e.g. by calling
   JS_GetPropertyValue() instead of the Const form -- double-frees here.

     case  targets
     ----  ----------------------------------------------------------------
     1     the key's valueOf reassigns the parameter that holds the key
     2     the same, but the parameter is CAPTURED, so the write goes through
           a JSVarRef aliasing &sf->arg_buf[idx]
     3     the key's toString allocates heavily during the conversion, so the
           borrowed key is live across collector activity
     4     integer keys on a dense array -- the inline fast path
     5     out-of-range, negative, fractional and hole keys
     6     string keys, prototype chain, missing property, numeric-string key
     7     non-object receivers must throw or box exactly as the pair did
     8     typed arrays and Arguments receivers, which the inline path declines
     9     the receiver is a Proxy whose get trap reassigns the caller's frame
     10    the array is demoted from fast_array to a generic object between
           loads (delete, then a huge index), with the same key parameter
     11    `recv[keyParam]()` is OP_get_array_el2 and must NOT be fused
     12    the key parameter index must not be truncated to one byte
     13    a key whose toString throws -- the exception must propagate and the
           receiver on the stack must still be released
     14    an `arguments`-elided site (patch 0048): arguments[i] where i is a
           parameter, which is OP_get_arg_el and must not be confused with
           this opcode
     15    several parameters, so a wrong slot index names a different key
     16    the fast path must read arg_buf, not var_buf, at the SAME index --
           int locals against numeric-string key parameters
     17    Symbol keys, and a key that is `undefined` / `null`
*/

function run() {
  var out = [];

  /* 1: the key's valueOf clears the parameter that holds the key. */
  (function () {
    function f(o, k) {
      return o[k];
    }
    var k = { toString: function () { return 'A'; } };
    /* reassigning the *caller's* variable does not touch the callee frame, so
       do it from inside the conversion via a second function that owns the
       parameter. */
    function g(o, k) {
      var kk = k;
      k = null;          // writes arg_buf[1] while kk is still the borrowed key
      return o[kk];
    }
    out.push('1a:' + f({ A: 'A' }, k));
    out.push('1b:' + g({ A: 'A' }, 'A'));

    function h(o, k) {
      return o[k];
    }
    var self = {
      toString: function () {
        /* nothing here can reach h's frame; the point of this arm is that the
           conversion runs user code at all while the key is borrowed. */
        var junk = [];
        for (var i = 0; i < 50; i++) junk.push({ i: i });
        return 'A';
      }
    };
    out.push('1c:' + h({ A: 'A' }, self));
  })();

  /* 2: the key parameter is CAPTURED, so a write to it goes through a JSVarRef
        that aliases &sf->arg_buf[idx]. The closure is invoked from inside the
        key's own valueOf, i.e. while the key is borrowed. */
  (function () {
    function f(o, k) {
      var clear = function () { k = 'B'; };
      var probe = {
        toString: function () { clear(); return 'A'; }
      };
      /* first load: k is the probe object; its toString overwrites the captured
         parameter mid-instruction. The borrowed key must survive. */
      var first = o[k === undefined ? 'A' : k];
      k = probe;
      var second = o[k];
      return first + ';' + second + ';' + k;
    }
    out.push('2:' + f({ A: 'A', B: 'B' }, 'A'));
  })();

  /* 3: the key's toString allocates heavily, so the borrowed key is live across
        collector activity. */
  (function () {
    function f(o, k) {
      return o[k];
    }
    var k = {
      toString: function () {
        var acc = [];
        for (var i = 0; i < 2000; i++) acc.push({ a: i, b: 'x' + i });
        return 'A';
      }
    };
    var s = '';
    for (var i = 0; i < 5; i++) s += f({ A: 'A' }, k);
    out.push('3:' + s);
  })();

  /* 4: integer keys, dense array receiver -- the inline fast path. */
  (function () {
    function f(a, i) { return a[i]; }
    var a = [10, 20, 30, 40], s = '', i;
    for (i = 0; i < a.length; i++) s += f(a, i) + ',';
    out.push('4:' + s);
  })();

  /* 5: out-of-range, negative, fractional and HOLE keys. The hole test in the
        inline body is what makes a hole fall through to the prototype chain
        instead of returning the uninitialized slot. */
  (function () {
    function f(a, i) { return a[i]; }
    var a = [1, 2, 3];
    var holey = [0, 1, 2, 3];
    delete holey[2];
    Object.prototype[2] = 'FROM-PROTO';
    var got = [
      String(f(a, 5)), String(f(a, -1)), String(f(a, 1.5)),
      String(f(holey, 2)), String(f(holey, 3))
    ];
    delete Object.prototype[2];
    out.push('5:' + got.join(';'));
  })();

  /* 6: string keys, prototype chain, missing property, numeric-string key. */
  (function () {
    function Base() {}
    Base.prototype.inherited = 'proto';
    function f(o, k) { return o[k]; }
    var obj = new Base();
    obj.own = 'own';
    var arr = [7, 8, 9];
    out.push('6:' + f(obj, 'own') + ';' + f(obj, 'inherited') + ';' +
             f(obj, 'absent') + ';' + f(arr, '1') + ';' + f(arr, 'length'));
  })();

  /* 7: non-object receivers. */
  (function () {
    function f(o, k) { return o[k]; }
    var got = [];
    got.push(String(f('hello', 'length')));
    got.push(String(f('hello', 1)));
    got.push(String(f(42, 'toFixed') === Number.prototype.toFixed));
    got.push(String(f(true, 'constructor') === Boolean));
    try { got.push(String(f(null, 'x'))); } catch (e) { got.push(e.constructor.name); }
    try { got.push(String(f(undefined, 'x'))); } catch (e) { got.push(e.constructor.name); }
    out.push('7:' + got.join(';'));
  })();

  /* 8: typed array and Arguments receivers -- the inline dense path declines
        these (class_id != JS_CLASS_ARRAY) and they take the generic call. */
  (function () {
    function f(a, i) { return a[i]; }
    function g(i) { return arguments[i]; }
    out.push('8:' + f(new Int32Array([7, 8, 9]), 1) + ';' +
             f(new Float64Array([1.5, 2.5]), 1) + ';' +
             f(new Uint8Array([1, 2]), 9) + ';' +
             g(1, 'q', 'r'));
  })();

  /* 9: the receiver is a Proxy whose get trap writes to the caller's frame. The
        trap runs strictly AFTER the key has been converted to an atom, but a
        build that kept the borrowed key live across it would still be wrong. */
  (function () {
    var box = { k: 'A' };
    function f(o, k) { return o[k]; }
    var target = { A: 'A', B: 'B' };
    var p = new Proxy(target, {
      get: function (t, prop, r) {
        box.k = 'CLOBBERED';
        var junk = [];
        for (var i = 0; i < 200; i++) junk.push([i]);
        return typeof prop === 'string' ? 'proxy:' + prop : 'proxy:sym';
      }
    });
    out.push('9:' + f(p, box.k) + ';' + f(p, 3) + ';' + box.k);
  })();

  /* 10: the receiver is demoted from fast_array to a generic object between two
         loads through the same key parameter. */
  (function () {
    function f(a, i) { return a[i]; }
    var a = [1, 2, 3];
    var before = f(a, 1);
    a[100000] = 'far';          // forces convert_fast_array_to_array
    var after = f(a, 1);
    out.push('10:' + before + ';' + after + ';' + f(a, 100000) + ';' + f(a, 50));
  })();

  /* 11: `recv[keyParam]()` compiles to OP_get_array_el2, which KEEPS the
         receiver on the stack for the call and has a different stack effect.
         The peephole must not match it. */
  (function () {
    function f(o, m) { return o[m]('x') + ';' + o[m]('y'); }
    out.push('11:' + f({ tag: 'T', greet: function (s) { return this.tag + s; } }, 'greet'));
  })();

  /* 12: the argument index is ONE BYTE, so the peephole must decline when it is
         >= 256 and let the general 3-byte OP_get_arg run instead. p260 holds the
         string "hit"; a truncated index (260 & 0xff == 4) would name p4, which
         holds the string "WRONG". Built with eval so the 300 bindings are real
         parameters of a real function. */
  (function () {
    var i, params = [], args = [];
    for (i = 0; i < 300; i++) { params.push('p' + i); args.push('n' + i); }
    var src = '(function (recv,' + params.join(',') + ') { return recv[p260]; })';
    var f = eval(src);
    args[260] = 'hit';
    args[4] = 'WRONG';
    out.push('12:' + f.apply(null, [{ hit: 'ok-260', WRONG: 'BAD-4' }].concat(args)));
  })();

  /* 13: a key whose toString throws. */
  (function () {
    function f(o, k) { return o[k]; }
    var k = { toString: function () { throw new RangeError('nope'); } };
    var got;
    try { got = f({}, k); } catch (e) { got = e.constructor.name + ':' + e.message; }
    out.push('13:' + got);
  })();

  /* 14: an `arguments`-elided site (patch 0048). `arguments[i]` with i a
         parameter is OP_get_arg_el, a DIFFERENT opcode that reads the frame
         slots directly; this case exists so a confusion between the two shows
         up as a wrong answer rather than as silence. */
  (function () {
    function f(i) { return arguments[i]; }
    function g(i) { var a = arguments; return a[i] + ';' + a.length; }
    out.push('14:' + f(1, 'x', 'y') + ';' + f(0, 'x') + ';' + String(f(9, 'x')) +
             ';' + g(2, 'p', 'q', 'r'));
  })();

  /* 15: several key parameters, so a wrong slot index names a different key. */
  (function () {
    function f(o, k0, k1, k2, k3) {
      return o[k0] + o[k1] + o[k2] + o[k3];
    }
    out.push('15:' + f({ a: 'A', b: 'B', c: 'C', d: 'D' }, 'a', 'b', 'c', 'd'));
  })();

  /* 16: THE REGISTER FILE MUST BE arg_buf, NOT var_buf, and the corpus has to
         be able to tell them apart at the SAME index. Added after mutation M3 --
         which changed only the fast-path tag test from arg_buf[idx] to
         var_buf[idx], leaving the value read as arg_buf[idx] -- SURVIVED the
         first version of this file. That mutant is a weakened guard rather than
         a wrong answer: it misfires only when var_buf[idx] is an INT while
         arg_buf[idx] is not, at which point JS_VALUE_GET_INT() is applied to a
         string pointer. Nothing else here put an int local and a non-int key
         parameter at the same slot index with a dense array receiver.

         Every key parameter is a NUMERIC STRING, which is a valid index for the
         array but is not JS_TAG_INT, so the correct engine always takes the
         generic call. Every local is an int, at every index a key parameter can
         occupy, so a build that tests the wrong register file finds JS_TAG_INT
         and takes the inline dense path on a string. */
  (function () {
    function f(a, k0, k1, k2, k3, k4, k5) {
      var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5, v6 = 6, v7 = 7;
      var acc = a[k0] + ';' + a[k1] + ';' + a[k2] + ';' + a[k3] + ';' +
                a[k4] + ';' + a[k5];
      return acc + ';' + (v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7);
    }
    out.push('16:' + f(['z', 'y', 'x', 'w', 'v', 'u'],
                       '0', '1', '2', '3', '4', '5'));

    /* the same shape with an object receiver, so the wrong-register-file build
       cannot be rescued by the class_id test. */
    function g(o, k0, k1) {
      var w0 = 0, w1 = 1, w2 = 2;
      return o[k0] + ';' + o[k1] + ';' + (w0 + w1 + w2);
    }
    out.push('16b:' + g({ '0': 'ZERO', '1': 'ONE' }, '0', '1'));

    /* 16c is what actually discriminates a wrong register file, and the first
       two arms above do NOT. A string key mis-tagged as INT still fails the
       `(uint32_t)JS_VALUE_GET_INT(key) < count` bound, because the int32 half
       of a JSString pointer is a large number -- so the wrong-buffer build
       falls back to the generic call and answers correctly by accident. The
       tags whose PAYLOAD is a small integer are the ones that get through:
       JS_UNDEFINED and JS_NULL are JS_MKVAL(tag, 0) and `true` is
       JS_MKVAL(JS_TAG_BOOL, 1). With an int local at the same slot index and a
       dense array receiver whose elements 0 and 1 exist, a build that tests
       var_buf for the INT tag and then reads arg_buf's payload returns a[0] or
       a[1] instead of a['undefined'] / a['null'] / a['true']. */
    function h(a, k0, k1, k2) {
      var n0 = 0, n1 = 1, n2 = 2, n3 = 3;
      return a[k0] + ';' + a[k1] + ';' + a[k2] + ';' + (n0 + n1 + n2 + n3);
    }
    var dense = ['ELEM0', 'ELEM1', 'ELEM2'];
    dense['undefined'] = 'U';
    dense['null'] = 'N';
    dense['true'] = 'T';
    out.push('16c:' + h(dense, undefined, null, true));
  })();

  /* 17: Symbol keys, and keys that are undefined / null / booleans. */
  (function () {
    var S = Symbol('s');
    var o = {};
    o[S] = 'SYM';
    o['undefined'] = 'U';
    o['null'] = 'N';
    o['true'] = 'T';
    function f(x, k) { return x[k]; }
    out.push('17:' + f(o, S) + ';' + f(o, undefined) + ';' + f(o, null) + ';' +
             f(o, true));
  })();

  print(out.join('\n'));
}

run();
