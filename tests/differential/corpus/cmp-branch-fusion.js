/* Adversarial corpus for the round-4 compare-and-branch fusion
   (docs/opcode-space-and-three-address.md).

   The mechanism under test: `resolve_labels` folds a comparison
   (lt/lte/gt/gte/eq/neq/strict_eq/strict_neq) and the conditional branch that
   immediately consumes its result into one instruction, OP_cmp_br8 or
   OP_cmp_br, carrying a subop byte that names the comparison and the branch
   sense.

   ## What each guard is discriminated by

     guard                                     detected by
     ----------------------------------------  ---------------------------
     subop selects the right comparison        cases 1, 2 (all eight ops,
                                               across <, ==, > orderings)
     subop's sense bit                         cases 3, 4 (both senses of
                                               every comparison)
     sense bit follows the if_false/goto
       INVERSION rewrite in resolve_labels     case 5 (`a < b ? x : y` and
                                               short-circuit chains, which is
                                               where `op ^= if_true ^ if_false`
                                               fires)
     a jump target between the comparison and
       the branch cancels the fusion           case 20 ONLY, and that was
                                               established by mutation test
                                               rather than by reading the
                                               parser. Cases 6, 7 and 12 look
                                               like they test it and DO NOT:
                                               a build with the guard deleted
                                               passes all three. The construct
                                               that discriminates is a
                                               comparison on the right of
                                               `||` / `&&` --
                                               `(a < b || c < d)`, not
                                               `(a < b || c)`
     the comparison result is USED, not
       branched on, so no fusion               case 8
     displacement base is pos + 2, not pos + 1 cases 9, 10 (forward short,
                                               forward long >127 bytes) and
                                               case 11 (backward, loop edges)
     long form shrinks to the short form
       correctly (opcode byte is TWO bytes
       before the displacement)                case 10
     slow path leaves the boolean where the
       fused handler reads it                  case 13 (strings, doubles,
                                               valueOf objects, null/NaN)
     exception out of a slow comparison
       unwinds with the stack consistent       case 14
     compute_stack_size explores the branch
       target (a missing case here is a SILENT
       miscompile: too small a stack)          case 21 ONLY. Case 15 does NOT
                                               discriminate it: it puts the
                                               deep expression on the
                                               FALL-THROUGH side, which is
                                               measured whether or not the
                                               branch target is explored. The
                                               discriminating shape is
                                               `if (a < b) return 0;` followed
                                               by a wide expression, so the
                                               peak is reachable only through
                                               the target
     generator save/restore across a fused
       branch                                  case 16

   ## What is NOT discriminated by output, and why

   The `cmp_emit_pos >= 0 && bc_out.size == cmp_emit_pos + 1` adjacency check
   has a defence-in-depth half: `bc_out.size == cmp_emit_pos + 1` can only be
   false if something was emitted in between, and everything that emits also
   clears the pending state. Deleting that half changes no output on any input
   reachable today. It is kept because it is what makes "the comparison is
   still the last thing emitted" true by construction rather than by audit.
*/
function run() {
  var out = [];

  /* 1-2: every comparison, int fast path, three operand orderings. */
  function all(a, b, tag) {
    var r = [];
    if (a < b) r.push('lt');
    if (a <= b) r.push('lte');
    if (a > b) r.push('gt');
    if (a >= b) r.push('gte');
    if (a == b) r.push('eq');
    if (a != b) r.push('neq');
    if (a === b) r.push('seq');
    if (a !== b) r.push('sneq');
    out.push(tag + ':' + r.join(','));
  }
  all(1, 2, '1a'); all(2, 2, '1b'); all(3, 2, '1c');
  all(-1, 0, '2a'); all(0, -1, '2b'); all(2147483647, -2147483648, '2c');

  /* 3-4: the other branch sense for every comparison. */
  function alln(a, b, tag) {
    var r = [];
    if (!(a < b)) r.push('lt');
    if (!(a <= b)) r.push('lte');
    if (!(a > b)) r.push('gt');
    if (!(a >= b)) r.push('gte');
    if (!(a == b)) r.push('eq');
    if (!(a != b)) r.push('neq');
    if (!(a === b)) r.push('seq');
    if (!(a !== b)) r.push('sneq');
    out.push(tag + ':' + r.join(','));
  }
  alln(1, 2, '3a'); alln(2, 2, '3b'); alln(3, 2, '3c');

  (function () {
    var r = [];
    for (var i = 0; i < 3; i++)
      for (var j = 0; j < 3; j++)
        r.push(i < j ? 'L' : (i > j ? 'G' : 'E'));
    out.push('4:' + r.join(''));
  })();

  /* 5: the if_false(l1) goto(l2) label(l1) -> if_false(l2) inversion, which is
        the rewrite that flips `op` after the comparison has been emitted. */
  (function () {
    var r = [];
    for (var i = 0; i < 4; i++) {
      if (i < 2) { r.push('a' + i); } else { r.push('b' + i); }
      var t = (i !== 2) ? 'x' : 'y';
      r.push(t);
    }
    out.push('5:' + r.join(','));
  })();

  /* 6-7: short-circuit operators put a jump target next to the branch. */
  function f6(a, b, c) { return (a < b || c) ? 'T' : 'F'; }
  out.push('6:' + [f6(1, 2, 0), f6(2, 1, 1), f6(2, 1, 0), f6(1, 2, 1)].join(','));
  function f7(a, b, c) { return (a < b && c) ? 'T' : 'F'; }
  out.push('7:' + [f7(1, 2, 1), f7(1, 2, 0), f7(2, 1, 1), f7(2, 1, 0)].join(','));

  /* 8: the comparison result is stored, returned and printed -- never branched
        on -- so the fusion must not fire and the value must survive. */
  (function () {
    var r = [];
    for (var i = 0; i < 3; i++) { var v = i < 1; r.push(typeof v + '/' + v); }
    r.push(String(1 < 2), String(2 < 1), String([1, 2, 3].filter(function (x) { return x > 1; })));
    out.push('8:' + r.join(','));
  })();

  /* 9: forward short displacement. */
  function f9(a, b) { if (a < b) { return 'in'; } return 'out'; }
  out.push('9:' + f9(1, 2) + ',' + f9(2, 1));

  /* 10: forward LONG displacement -- the body is deliberately more than 127
         bytes of bytecode, so the branch cannot use the 8-bit form and the
         shrink pass must leave the 6-byte form alone. */
  function f10(a, b) {
    var r = [];
    if (a < b) {
      r.push('p00'); r.push('p01'); r.push('p02'); r.push('p03');
      r.push('p04'); r.push('p05'); r.push('p06'); r.push('p07');
      r.push('p08'); r.push('p09'); r.push('p10'); r.push('p11');
      r.push('p12'); r.push('p13'); r.push('p14'); r.push('p15');
      r.push('p16'); r.push('p17'); r.push('p18'); r.push('p19');
      r.push('p20'); r.push('p21'); r.push('p22'); r.push('p23');
      r.push('p24'); r.push('p25'); r.push('p26'); r.push('p27');
      r.push('p28'); r.push('p29'); r.push('p30'); r.push('p31');
      r.push('p32'); r.push('p33'); r.push('p34'); r.push('p35');
      r.push('p36'); r.push('p37'); r.push('p38'); r.push('p39');
    }
    r.push('end');
    return r.join('');
  }
  out.push('10:' + f10(1, 2).length + ',' + f10(2, 1));

  /* 11: backward displacements -- every loop form. */
  (function () {
    var s = 0, i;
    for (i = 0; i < 50; i++) s += i;
    var j = 0; while (j !== 9) j++;
    var k = 12; do { k--; } while (k > 3);
    var m = 0; for (;;) { m++; if (m >= 7) break; }
    out.push('11:' + s + ',' + j + ',' + k + ',' + m);
  })();

  /* 12: a jump target BETWEEN the comparison and its branch. The `continue`
         target lands there, so the fusion must be cancelled by OP_label. */
  (function () {
    var r = [];
    for (var i = 0; i < 6; i++) {
      if (i % 2) continue;
      if (i > 2) r.push('h' + i); else r.push('l' + i);
    }
    out.push('12:' + r.join(','));
  })();

  /* 13: the slow path -- every non-int-pair operand kind. */
  (function () {
    var r = [];
    r.push('a' < 'b'); r.push('b' < 'a'); r.push('abc' < 'abd');
    r.push(1.5 < 2.5); r.push(2.5 <= 2.5); r.push(-0 === 0); r.push(1 / 0 > 0);
    r.push(null == undefined); r.push(null === undefined);
    r.push(NaN !== NaN); r.push(NaN < 1); r.push(NaN >= 1);
    var vo = { valueOf: function () { return 5; } };
    r.push(vo < 6); r.push(vo >= 5); r.push(vo == 5); r.push(vo === 5);
    r.push([] == false); r.push('1' == 1); r.push('1' === 1);
    r.push(true < 2); r.push(undefined > 0);
    var big = 9007199254740993;
    r.push(big > 9007199254740992);
    var res = [];
    for (var i = 0; i < r.length; i++) res.push(String(r[i]));
    out.push('13:' + res.join(','));
  })();

  /* 14: an exception thrown out of a fused slow comparison. */
  (function () {
    var r = [];
    var t = { valueOf: function () { throw new Error('boom'); } };
    try { if (t < 1) r.push('taken'); else r.push('nottaken'); }
    catch (e) { r.push('caught:' + e.message); }
    try { if (t === 1) r.push('taken2'); else r.push('nottaken2'); }
    catch (e) { r.push('caught2:' + e.message); }
    var s = Symbol('s');
    try { if (s < 1) r.push('sym'); } catch (e) { r.push('symcaught:' + e.constructor.name); }
    out.push('14:' + r.join(','));
  })();

  /* 15: a deep expression behind a fused branch. If compute_stack_size did not
         explore the branch target the function would be allocated too small a
         stack and this would corrupt memory rather than print. */
  (function () {
    function deep(a, b) {
      if (a < b) {
        return [[a, b, [a + b, a - b, [a * b, [a, [b, [a, [b, [a + 1]]]]]]]]]
          .concat([[b, a]]).join('|');
      }
      return 'no';
    }
    out.push('15:' + deep(1, 2) + ';' + deep(2, 1));
  })();

  /* 16: a fused branch inside a generator, across a yield. */
  (function () {
    function gen(n) {
      var i = 0;
      return {
        next: function () {
          if (i < n) { return { value: i++, done: false }; }
          return { value: undefined, done: true };
        }
      };
    }
    var g = gen(4), r = [], v;
    while (!(v = g.next()).done) r.push(v.value);
    out.push('16:' + r.join(','));
  })();

  /* 17: recursion, so the fused handler's frame is multiplied by depth. */
  (function () {
    function fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
    function ack(m, n) {
      if (m === 0) return n + 1;
      if (n === 0) return ack(m - 1, 1);
      return ack(m - 1, ack(m, n - 1));
    }
    out.push('17:' + fib(20) + ',' + ack(2, 3));
  })();

  /* 18: comparisons inside try/finally, where the branch target sits behind a
         catch boundary and catch_pos bookkeeping applies. */
  (function () {
    var r = [];
    for (var i = 0; i < 4; i++) {
      try {
        if (i > 1) throw new Error('e' + i);
        r.push('ok' + i);
      } catch (e) {
        r.push(e.message);
      } finally {
        if (i === 3) r.push('fin');
      }
    }
    out.push('18:' + r.join(','));
  })();

  /* 20: THE construct that puts a jump target between a comparison and the
         branch that consumes it -- a comparison on the right-hand side of
         `||` or `&&`, where the short-circuit join label lands there.

         This case exists because cases 6, 7 and 12 do NOT discriminate that
         guard: a build with the `cmp_emit_pos = -1` in `case OP_label:`
         deleted passed all of them. Found by probing constructs against the
         `label_cancelled` counter -- `(a < b || c)` never triggers it,
         `(a < b || c < d)` triggers it once. The guard fires 1010 times on
         bench/fixtures/rn-metro-ios-prod.js, so it is load-bearing, not
         theoretical. */
  (function () {
    function f(a, b, c, d) { if (a < b || c < d) return 'T'; return 'F'; }
    function g(a, b, c, d) { if (a < b && c < d) return 'T'; return 'F'; }
    function h(a, b, c, d) { return (a >= b || c === d) ? 'T' : 'F'; }
    function k(a, b, c, d, e, f2) { if (a < b || c < d || e < f2) return 'T'; return 'F'; }
    var r = [];
    var args = [[1, 2, 3, 4], [2, 1, 3, 4], [1, 2, 4, 3], [2, 1, 4, 3]];
    for (var i = 0; i < args.length; i++) {
      var x = args[i];
      r.push(f(x[0], x[1], x[2], x[3]));
      r.push(g(x[0], x[1], x[2], x[3]));
      r.push(h(x[0], x[1], x[2], x[3]));
      r.push(k(x[0], x[1], x[2], x[3], x[0], x[3]));
    }
    out.push('20:' + r.join(''));
  })();

  /* 21: a function whose MAXIMUM stack depth is reachable only through the
         fused branch's target. `if (a < b) return 0;` compiles to
         cmp / branch-to-L / return 0 / L: <deep>, and the fall-through path
         ends in a return, so the deep expression is visited by
         compute_stack_size ONLY if the branch target is explored.

         This case exists because case 15 did NOT discriminate that guard --
         it put the deep expression on the fall-through side, where the stack
         is measured anyway. A build with compute_stack_size's OP_cmp_br8 case
         deleted passed case 15 and fails this one. The failure mode round 2
         documented is a stack allocated too small, which corrupts memory
         rather than raising. */
  (function () {
    function deepIfNotTaken(a, b) {
      if (a < b) return 0;
      return [1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
              11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
              21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
              31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
              41, 42, 43, 44, 45, 46, 47, 48, 49, 50,
              51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
              61, 62, 63, 64, 65, 66, 67, 68, 69, 70,
              71, 72, 73, 74, 75, 76, 77, 78, 79, 80].length;
    }
    function deepLoop(n) {
      var t = 0, i;
      for (i = 0; i < n; i++) {
        if (i !== 3) { t += 1; continue; }
        t += [1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
              11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
              21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
              31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
              41, 42, 43, 44, 45, 46, 47, 48, 49, 50].length;
      }
      return t;
    }
    out.push('21:' + deepIfNotTaken(1, 2) + ',' + deepIfNotTaken(2, 1) +
             ',' + deepLoop(6));
  })();

  /* 19: switch statements, which lower to chains of strict comparisons. */
  (function () {
    function sw(x) {
      switch (x) {
        case 0: return 'zero';
        case 1: return 'one';
        case 'a': return 'letter';
        case null: return 'null';
        default: return 'other';
      }
    }
    var r = [];
    var vals = [0, 1, 'a', null, undefined, 2, '0'];
    for (var i = 0; i < vals.length; i++) r.push(sw(vals[i]));
    out.push('19:' + r.join(','));
  })();

  print(out.join('\n'));
}

run();
