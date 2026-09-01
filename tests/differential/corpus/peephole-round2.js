/* Adversarial corpus for the fusion-round-2 FREE peepholes
   (docs/opcode-fusion-round-2.md, patch 0022):

     A.  dup put_loc_check(n)     drop  ->  put_loc_check(n)
     A'. dup put_var_ref_check(n) drop  ->  put_var_ref_check(n)
     B.  lnot if_false(l)               ->  if_true(l)
         lnot if_true(l)                ->  if_false(l)

   ## COVERAGE FIRST — read this before adding a case

   Patch 0020 shipped a corpus file that exercised its optimization ZERO times,
   because every variable in it was declared at top level and a top-level `var`
   in a script is a GLOBAL, so `o[k]` compiled to OP_get_var and the peephole
   never fired. The file passed identically against two deliberately broken
   builds. Everything here therefore runs inside a function, and:

     - transformation A needs a TDZ-CHECKED store, i.e. a `let`/`const` binding
       whose write the compiler cannot prove is past its initialization. A
       plain `var` compiles to OP_put_loc and is handled by the PRE-EXISTING
       `dup put_x(n) drop -> put_x(n)` peephole, not by this one.
     - transformation A' additionally needs the binding CAPTURED by a closure,
       so it becomes a var_ref rather than a local.
     - transformation B needs `!expr` in a BRANCH position (`if`, `while`, `?:`,
       `&&`/`||` used as a test), not `var b = !expr`.

   Coverage was verified with a -DQJS_FUSE2_STATS build, which counts how many
   times each transformation fires in resolve_labels. This file fires both.

   ## What each case is trying to break

     case  targets
     ----  ----------------------------------------------------------------
     1-3   A/A' fire at all, and the stored value is the right one
     4     the assignment's VALUE is still produced when it is NOT dropped
           (the peephole must not fire; if it did, `y` would be undefined)
     5     chained assignment — the middle value must survive
     6     TDZ still throws, and throws BEFORE the store, at the same point
     7     const reassignment still throws TypeError
     8     the throw happens even though the peephole deleted the surrounding
           dup/drop, and the catch still sees a sane stack
     9-12  B: both branch polarities, and truthiness of every falsy value
     13    B must not change WHICH value is consumed: `!a && b`
     14    ToBoolean must not call valueOf/toString — an object with both is
           always truthy, and neither hook may run
     15    `!!x` in branch position: the inner lnot pair must still work out
     16    lnot whose branch is a jump TARGET cannot fuse (a label sits between
           them); the answer must be right either way
*/

function run() {
  var out = [];

  /* 1: dup put_loc_check drop — a `let` written after a conditional
        initialization, so the compiler keeps the TDZ check. */
  (function () {
    var i, r = [];
    for (i = 0; i < 3; i++) {
      let v;
      if (i > 0) v = i * 10;
      v = i + 1;          /* expression statement -> dup put_loc_check drop */
      r.push(v);
    }
    out.push('1:' + r.join(','));
  })();

  /* 2: dup put_var_ref_check drop — same, but captured by a closure. */
  (function () {
    var fns = [];
    for (var i = 0; i < 3; i++) {
      let c;
      c = i * 2;          /* captured below -> put_var_ref_check */
      fns.push(function () { return c; });
      c = c + 1;
    }
    out.push('2:' + fns.map(function (f) { return f(); }).join(','));
  })();

  /* 2b: the real dup put_var_ref_check drop. Case 2 above does NOT produce
        one — measured: `let c` written in the same function that declares it
        compiles to put_loc_check even when an inner function READS it. A
        var_ref store needs an inner function that WRITES the outer binding.
        Added after mutation A2 (emit OP_put_loc_check regardless of which
        store was matched) survived the corpus: without this case the file
        never exercised the var_ref arm at all. */
  (function () {
    let c;
    function setit(v) { c = v; }
    var r = [];
    for (var i = 0; i < 3; i++) { setit(i * 2); r.push(c); }
    out.push('2b:' + r.join(','));
  })();

  /* 3: many stores in sequence; the last one wins. */
  (function () {
    let a;
    a = 1; a = 2; a = 3;
    out.push('3:' + a);
  })();

  /* 4: the value of the assignment is USED, so the drop is absent and the
        peephole must not fire. If it fired anyway, y would be undefined. */
  (function () {
    let x;
    var y = (x = 7);
    out.push('4:' + x + ';' + y);
  })();

  /* 5: chained assignment through a checked binding. */
  (function () {
    let p, q;
    p = q = 5;
    out.push('5:' + p + ';' + q);
  })();

  /* 6: TDZ read-before-write, and TDZ write-before-init. Both must throw
        ReferenceError, and nothing may be stored. */
  (function () {
    var got = [];
    try { (function () { z = 1; let z; })(); got.push('no-throw'); }
    catch (e) { got.push(e.constructor.name); }
    try { (function () { let w = w + 1; })(); got.push('no-throw'); }
    catch (e) { got.push(e.constructor.name); }
    out.push('6:' + got.join(','));
  })();

  /* 7: const reassignment. */
  (function () {
    var got;
    try { (function () { const k = 1; k = 2; })(); got = 'no-throw'; }
    catch (e) { got = e.constructor.name; }
    out.push('7:' + got);
  })();

  /* 8: the throw must leave the surrounding expression stack consistent —
        a value computed before the failing store must not leak into the
        catch or corrupt the following statements. */
  (function () {
    var seen = [];
    function boom() { seen.push('side'); return 42; }
    try {
      (function () { u = boom(); let u; })();
    } catch (e) { seen.push(e.constructor.name); }
    seen.push('after');
    out.push('8:' + seen.join(','));
  })();

  /* 9-12: lnot in branch position, both polarities, all falsy values. */
  (function () {
    var vals = [0, -0, '', null, undefined, NaN, false, 1, 'x', [], {}];
    var a = [], b = [], c = [];
    for (var i = 0; i < vals.length; i++) {
      var v = vals[i];
      if (!v) a.push(i);                       /* lnot if_false -> if_true */
      while (!v) { b.push(i); break; }
      c.push(!v ? 'T' : 'F');
    }
    out.push('9:' + a.join(','));
    out.push('10:' + b.join(','));
    out.push('11:' + c.join(''));
  })();

  (function () {
    var r = [];
    var i = 0;
    do { r.push(i); i++; } while (!(i >= 3));   /* lnot if_true -> if_false */
    out.push('12:' + r.join(','));
  })();

  /* 13: !a && b — the lnot must consume `a` and only `a`. */
  (function () {
    var r = [];
    var pairs = [[0, 1], [0, 0], [1, 1], [1, 0], ['', 'z'], ['q', 'z']];
    for (var i = 0; i < pairs.length; i++) {
      if (!pairs[i][0] && pairs[i][1]) r.push(i);
    }
    out.push('13:' + r.join(','));
  })();

  /* 14: ToBoolean must not run valueOf or toString. An object is always
        truthy; if either hook ran, `hooks` would be non-empty and the
        branch could even take the wrong arm. */
  (function () {
    var hooks = [];
    var obj = {
      valueOf: function () { hooks.push('valueOf'); return 0; },
      toString: function () { hooks.push('toString'); return ''; }
    };
    var taken = 'else';
    if (!obj) taken = 'then';
    out.push('14:' + taken + ';' + hooks.length);
  })();

  /* 15: double negation in branch position. */
  (function () {
    var r = [];
    var vals = [0, 1, '', 'a', null];
    for (var i = 0; i < vals.length; i++) if (!!vals[i]) r.push(i);
    out.push('15:' + r.join(','));
  })();

  /* 16: the branch after the lnot is itself a jump target, so a label sits
        between them and the peephole CANNOT fire. Included so the file
        covers the un-fused path with the same shape of expression. */
  (function () {
    var r = [];
    for (var i = 0; i < 4; i++) {
      var t = i % 2;
      if (!(t || i > 2)) r.push('a' + i); else r.push('b' + i);
    }
    out.push('16:' + r.join(','));
  })();

  print(out.join('\n'));
}

run();
