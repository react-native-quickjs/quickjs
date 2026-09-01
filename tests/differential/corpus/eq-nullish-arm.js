/* Adversarial corpus for the nullish loose-equality inline arm
   (docs/eq-nullish-inline-arm.md).

   The mechanism under test: the `else` branch of the both-int fast path in
   OP_eq / OP_neq (macro OP_CMP_EQ) and in OP_cmp_br8 / OP_cmp_br subop 4/5
   resolves `x == null` / `x != undefined` by TAG ALONE, without entering the
   no_inline helper js_eq_slow().  Admission: both operands in
   {null, undefined} -> true; exactly one of them nullish -> false, UNLESS the
   other side is an [[IsHTMLDDA]] object (Annex B B.3.6).

   ## What each guard is discriminated by

     guard                                    detected by
     ---------------------------------------  ----------------------------
     BOTH operands are tested for nullish,     case 2 -- and specifically the
       not just the left one                   rows where the LEFT operand is
                                                a non-nullish refcounted value
                                                and the right is null.  A
                                                left-only admission returns
                                                `false` for `"" == undefined`
                                                correctly by luck but returns
                                                the wrong answer for
                                                `undefined == undefined`
                                                reached from the other side and
                                                for `1 == null` vs `null == 1`
                                                asymmetry -- see case 5, which
                                                is the row that actually fails
                                                the left_only mutant.
     null == undefined is TRUE                 case 1
     no ToPrimitive / ToNumber against a
       nullish operand: 0 == null, "" == null,
       false == null, [] == null are all FALSE case 2, case 6 (valueOf/toString
                                                side-effect counters must NOT
                                                fire)
     the is_neq polarity is applied            case 3 -- every pair is printed
                                                under both == and !=
     the arm runs at the OP_cmp_br site as
       well as the OP_CMP site                 case 4 -- the same table again
                                                in `if (...)` position, which
                                                patch 0046 fuses.  richards's
                                                traffic is 100% this site.
     an OBJECT operand is not confused with
       a nullish one                           case 5 (`({}) == null` false,
                                                `({}) == ({})` false,
                                                `o == o` true)
     strict equality is untouched              case 7
     document.all / [[IsHTMLDDA]]              NOT COVERED HERE and cannot be:
                                                the bit is only settable from C
                                                via JS_SetIsHTMLDDA and node
                                                has no equivalent to diff
                                                against.  test262's
                                                language/expressions/equals/
                                                *html-dda* files are the only
                                                thing that discriminates that
                                                guard, and they are load-bearing
                                                for this patch.

   Values are read out of arrays so nothing is constant-folded by the parser.
*/

var VALUES = [
  null, undefined, 0, -0, 1, -1, NaN, Infinity,
  "", "0", "null", "undefined", " ",
  false, true,
  0n, 1n
];
var NAMES = [
  "null", "undefined", "0", "-0", "1", "-1", "NaN", "Infinity",
  "''", "'0'", "'null'", "'undefined'", "' '",
  "false", "true",
  "0n", "1n"
];

/* `Object.create(null) == 0` genuinely throws in both engines (no
   Symbol.toPrimitive, no inherited valueOf/toString), so the coercing
   comparisons in case 5 are wrapped.  A nullish comparison must NEVER reach
   the throwing path -- that is itself part of what case 5 checks. */
function safe(f, a, b) { try { return "" + f(a, b); } catch (e) { return "THROW"; } }

function eqV(a, b) { return a == b; }
function neV(a, b) { return a != b; }
function seV(a, b) { return a === b; }
function snV(a, b) { return a !== b; }
/* branch position -- this is the shape patch 0046 fuses into OP_cmp_br */
function eqB(a, b) { if (a == b) return "T"; return "F"; }
function neB(a, b) { if (a != b) return "T"; return "F"; }
function seB(a, b) { if (a === b) return "T"; return "F"; }
function snB(a, b) { if (a !== b) return "T"; return "F"; }

print("== case 1: nullish against nullish, both sites, both polarities");
var NULLISH = [null, undefined];
var NULLISH_N = ["null", "undefined"];
for (var i = 0; i < NULLISH.length; i++) {
  for (var j = 0; j < NULLISH.length; j++) {
    var a = NULLISH[i], b = NULLISH[j];
    print(NULLISH_N[i] + " vs " + NULLISH_N[j] +
      "  ==:" + eqV(a, b) + " !=:" + neV(a, b) +
      " ===:" + seV(a, b) + " !==:" + snV(a, b) +
      "  br== :" + eqB(a, b) + " br!=:" + neB(a, b) +
      " br===:" + seB(a, b) + " br!==:" + snB(a, b));
  }
}

print("== case 2: every primitive against null and undefined, BOTH ORDERS");
for (var k = 0; k < VALUES.length; k++) {
  for (var n = 0; n < NULLISH.length; n++) {
    var v = VALUES[k], q = NULLISH[n];
    print(NAMES[k] + " " + NULLISH_N[n] +
      "  L==:" + eqV(v, q) + " L!=:" + neV(v, q) +
      "  R==:" + eqV(q, v) + " R!=:" + neV(q, v) +
      "  L===:" + seV(v, q) + " R===:" + seV(q, v));
  }
}

print("== case 3: the full primitive x primitive table under == and !=");
for (var p = 0; p < VALUES.length; p++) {
  var row = "";
  for (var r = 0; r < VALUES.length; r++) {
    row += (eqV(VALUES[p], VALUES[r]) ? "1" : "0");
    row += (neV(VALUES[p], VALUES[r]) ? "1" : "0");
  }
  print(NAMES[p] + " " + row);
}

print("== case 4: the same table in BRANCH position (the OP_cmp_br site)");
for (var p2 = 0; p2 < VALUES.length; p2++) {
  var row2 = "";
  for (var r2 = 0; r2 < VALUES.length; r2++) {
    row2 += eqB(VALUES[p2], VALUES[r2]);
    row2 += neB(VALUES[p2], VALUES[r2]);
  }
  print(NAMES[p2] + " " + row2);
}

print("== case 5: objects against nullish and against each other");
var o1 = {};
var o2 = {};
var arr = [];
var arr0 = [0];
var np = Object.create(null);
var re = /x/;
var fn = function () {};
var boxN = new Number(0);
var boxS = new String("");
var boxB = new Boolean(false);
var OBJS = [o1, o2, arr, arr0, np, re, fn, boxN, boxS, boxB];
var ONAMES = ["{}", "{}2", "[]", "[0]", "Object.create(null)", "/x/",
              "function", "new Number(0)", "new String('')", "new Boolean(false)"];
for (var q1 = 0; q1 < OBJS.length; q1++) {
  print(ONAMES[q1] +
    "  ==null:" + eqV(OBJS[q1], null) + " null==:" + eqV(null, OBJS[q1]) +
    "  ==undef:" + eqV(OBJS[q1], undefined) + " undef==:" + eqV(undefined, OBJS[q1]) +
    "  !=null:" + neV(OBJS[q1], null) + " !=undef:" + neV(OBJS[q1], undefined) +
    "  br==null:" + eqB(OBJS[q1], null) + " br!=undef:" + neB(OBJS[q1], undefined) +
    "  self==:" + eqV(OBJS[q1], OBJS[q1]) +
    "  vs{}:" + safe(eqV, OBJS[q1], o1) +
    "  ==0:" + safe(eqV, OBJS[q1], 0) + " =='':" + safe(eqV, OBJS[q1], "") +
    "  ===null:" + seV(OBJS[q1], null) + " ===undef:" + seV(OBJS[q1], undefined));
}

print("== case 6: no coercion may be invoked against a nullish operand");
var calls = 0;
var trap = {
  valueOf: function () { calls++; return null; },
  toString: function () { calls++; return "null"; }
};
print("trap == null   -> " + (trap == null) + "  calls=" + calls);
print("null == trap   -> " + (null == trap) + "  calls=" + calls);
print("trap == undef  -> " + (trap == undefined) + "  calls=" + calls);
print("trap != null   -> " + (trap != null) + "  calls=" + calls);
if (trap == null) { print("branch taken"); } else { print("branch not taken"); }
print("after branch    calls=" + calls);
/* the SAME object against a non-nullish operand MUST coerce */
print("trap == 0      -> " + safe(eqV, trap, 0) + "  calls=" + calls);

print("== case 7: strict equality against nullish is unchanged");
for (var s = 0; s < VALUES.length; s++) {
  print(NAMES[s] +
    "  ===null:" + seV(VALUES[s], null) + " ===undef:" + seV(VALUES[s], undefined) +
    "  !==null:" + snV(VALUES[s], null) +
    "  br===null:" + seB(VALUES[s], null) + " br!==undef:" + snB(VALUES[s], undefined));
}

print("== case 8: symbols (refcounted, non-object, must free correctly)");
var sym = Symbol("s");
var sym2 = Symbol("s");
print("sym==null:" + (sym == null) + " null==sym:" + (null == sym) +
      " sym!=undef:" + (sym != undefined) + " sym==sym:" + (sym == sym) +
      " sym==sym2:" + (sym == sym2) +
      " br: " + eqB(sym, null) + neB(sym, undefined));

print("== case 9: the arm must not leak -- churn refcounted operands in a loop");
var acc = 0;
for (var it = 0; it < 20000; it++) {
  var tmp = { i: it };
  var str = "s" + (it & 7);
  if (tmp == null) acc += 1;
  if (str != undefined) acc += 2;
  if (null == tmp) acc += 4;
  acc += (tmp == undefined) ? 8 : 0;
  acc += (str == null) ? 16 : 0;
}
print("acc=" + acc);

print("== case 10: property access that yields undefined, the real idiom");
var holder = { a: 1, b: null };
var keys = ["a", "b", "c"];
var out = "";
for (var h = 0; h < keys.length; h++) {
  var val = holder[keys[h]];
  out += (val == null) ? "N" : "V";
  out += (val != null) ? "v" : "n";
  if (val == undefined) out += "U"; else out += "-";
}
print(out);

/* == case 11: RELATIONAL sub-ops of the fused compare-branch, with a nullish
   operand.  ADDED BY LANE NULLISH-1, 2026-08-18, because the corpus was BLIND
   to a real guard.

   The fused OP_cmp_br8 / OP_cmp_br opcode carries EIGHT sub-ops in one handler:
   idx 0..3 are < <= > >=, idx 4/5 are == !=, idx 6/7 are === !==.  The nullish
   arm admits ONLY (idx|1)==5.  A mutant that widens that admission to every
   sub-op answers `null < 1` as FALSE (equality rules) instead of TRUE
   (ToNumber(null) === 0), and it PASSED both the rest of this file and the two
   Annex B html-dda test262 files.  Relational comparisons against a nullish
   operand, in branch position, are the only thing that discriminates it. */
print("== case 11: relational sub-ops against a nullish operand");
var RL = [null, undefined, 0, 1, -1, "0", "1", true, false];
var RN = ["null", "undef", "0", "1", "-1", '"0"', '"1"', "true", "false"];
function ltB(a, b) { if (a < b)  return "T"; return "f"; }
function leB(a, b) { if (a <= b) return "T"; return "f"; }
function gtB(a, b) { if (a > b)  return "T"; return "f"; }
function geB(a, b) { if (a >= b) return "T"; return "f"; }
for (var r = 0; r < RL.length; r++) {
  var line = RN[r] + " :";
  for (var q = 0; q < RL.length; q++) {
    line += " " + ltB(RL[r], RL[q]) + leB(RL[r], RL[q]) +
                  gtB(RL[r], RL[q]) + geB(RL[r], RL[q]);
  }
  print(line);
}
