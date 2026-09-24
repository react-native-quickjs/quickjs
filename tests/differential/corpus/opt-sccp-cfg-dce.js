/* Adversarial corpus for the opt-in bytecode optimizer: local constant
   propagation, CFG simplification and dead-code elimination, plus the
   constant-run folder they feed -- the pass behind `qjsc -O`.

   Every line is compared byte for byte against node on all three arms
   (source, plain bytecode, and `-O` bytecode), so each case discriminates the
   optimizer from a conformant engine rather than from an expectation written
   by the person who wrote the pass.

   ## What each case is for

     case                          discriminates
     ----------------------------  ----------------------------------------
     1  minus zero                 `0 * x` folded to the integer 0, which
                                   turns 1/(0*-3) from -Infinity into
                                   Infinity.  This is a real miscompile this
                                   corpus was written after; the shapes below
                                   keep the factors in locals, so they become
                                   constants only through propagation, which
                                   is the case a parser-level constant run
                                   does not already cover.
     2  the algebraic identities   x+0 -> x, x-0, x*1, x&-1, x|0, x^0,
                                   x<<0, !!x applied to a value that is NOT a
                                   proven int32: a string, an array, an object
                                   with valueOf, -0, NaN, Infinity, a BigInt,
                                   null, undefined.  Each rewritable identity
                                   changes the answer -- by coercion, by
                                   string concatenation, by -0, or by
                                   throwing where the identity does not.
                                   This is also the only case that observes
                                   COERCION: valueOf must still be called
                                   when a rewrite replaces an operation that
                                   called it.
     3  constant branches          the live arm runs and the dead arm does not,
                                   including a call and a throw in it.  The
                                   condition is built through a local, so the
                                   parser cannot fold it and the pass has to.
     4  joins                      a local constant on one path and not on
                                   another must be unknown after the join, not
                                   whatever the first path happened to leave;
                                   the same through a loop back edge and
                                   through a catch edge.
     5  dead stores                a store nothing reads goes; one a closure,
                                   eval, a compound assignment, a
                                   post-increment or an aliased argument
                                   reads stays.  set_loc keeps its value.
     6  capture / eval / arguments the three routes by which a local is
                                   visible outside the function body.
     7  control flow               the jump shapes the simplifier rewrites (a
                                   goto to the next instruction, a goto to a
                                   goto, both arms of a branch equal), and the
                                   ones it must leave alone: loops, labelled
                                   break and continue, switch fallthrough,
                                   try/catch/finally, throw, and a
                                   generator's yield.
     8  wide locals                get_loc8 / get_loc and put_loc8 / put_loc,
                                   which the short forms (loc0..loc3) do not
                                   reach: a function with more than 256 locals.
     9  the constant folder        what it may fold (int32 arithmetic and
                                   comparison) and the traps next to it:
                                   int32 overflow must become a double rather
                                   than wrap, integer division must not
                                   truncate, -0 must survive, and a string,
                                   BigInt or object operand must not be folded
                                   at all.
    10  stack shapes               the shuffles and temporaries that leave the
                                   operand stack at the depth the next block
                                   assumes.  A wrong depth is a crash or a
                                   wrong value, never a near miss.

   `with` is the one exclusion this corpus cannot reach: the corpus runs as a
   script in qjs and as an ES module under node, and a module is always
   strict, so a `with` statement is a parse error on the node side.  Strict
   mode is declared once at the top for the same reason -- sloppy-mode
   `arguments` aliasing would differ between the two engines for reasons that
   have nothing to do with the optimizer.
*/

"use strict";

/* Every value is printed, never asserted: node's output is the expectation.
   sh() has to make -0, BigInt and NaN visible, because those are exactly the
   values a wrong fold turns into something else. */
function sh(v) {
  var t = typeof v;
  if (t === "number") {
    if (v === 0 && 1 / v < 0) return "-0";
    return String(v);
  }
  if (t === "bigint") return String(v) + "n";
  if (t === "string") return JSON.stringify(v);
  if (t === "undefined") return "undefined";
  return String(v);
}
function tp(v) { return typeof v; }
function safe(label, f) {
  try { print(label + " = " + sh(f())); } catch (e) { print(label + " threw " + e.name); }
}
function safeT(label, f) {
  try { var r = f(); print(label + " = " + sh(r) + " :" + tp(r)); }
  catch (e) { print(label + " threw " + e.name); }
}

print("== case 1: minus zero, with the factors held in locals");
/* The reciprocal is the discriminator: 1/+0 is Infinity and 1/-0 is -Infinity,
   so a fold that loses the sign shows up as the wrong infinity. */
var MZ = [
  ["0 * (2-5)",   function () { var a = 0, b = 2 - 5; return a * b; }],
  ["(2-5) * 0",   function () { var a = 2 - 5, b = 0; return a * b; }],
  ["0 * -3",      function () { var a = 0, b = -3; return a * b; }],
  ["-3 * 0",      function () { var a = -3, b = 0; return a * b; }],
  ["0 * 3",       function () { var a = 0, b = 3; return a * b; }],
  ["-0 * 3",      function () { var a = -0, b = 3; return a * b; }],
  ["3 * -0",      function () { var a = 3, b = -0; return a * b; }],
  ["-0 * -3",     function () { var a = -0, b = -3; return a * b; }],
  ["-0 * 0",      function () { var a = -0, b = 0; return a * b; }],
  ["0 * 0",       function () { var a = 0, b = 0; return a * b; }],
  ["0 * (1-1)",   function () { var a = 0, b = 1 - 1; return a * b; }],
  ["(1-1) * -2",  function () { var a = 1 - 1, b = -2; return a * b; }],
  ["0 - 0",       function () { var a = 0, b = 0; return a - b; }],
  ["-0 + 0",      function () { var a = -0, b = 0; return a + b; }],
  ["0 + -0",      function () { var a = 0, b = -0; return a + b; }],
  ["-0 - 0",      function () { var a = -0, b = 0; return a - b; }],
  ["0 / -3",      function () { var a = 0, b = -3; return a / b; }],
  ["-0 / 3",      function () { var a = -0, b = 3; return a / b; }],
  ["0 % -3",      function () { var a = 0, b = -3; return a % b; }],
  ["-0 % 3",      function () { var a = -0, b = 3; return a % b; }]
];
var vi, oi;
for (vi = 0; vi < MZ.length; vi++) {
  var f = MZ[vi][1];
  var r = f();
  print("  " + MZ[vi][0] + " = " + sh(r) + "  recip=" + (1 / r) +
        "  isNegZero=" + Object.is(r, -0) + "  isPosZero=" + Object.is(r, 0));
}
/* Deeper: the constant only appears after several local-to-local copies, and
   a loop whose carried value is -0 rather than 0. */
function mzChain1() { var a = 2 - 5; var b = 0; var c = b; var d = c; return d * a; }
function mzChain2() { var a = -3; var b = 0; var c = a; var d = b; var e = d; return e * c; }
function mzChain3() { var k = 0; for (var i = 0; i < 3; i++) k = k * -2; return k; }
function mzChain4() { var k = 0, neg = -2; for (var i = 0; i < 4; i++) k = k * neg; return k; }
print("  chain1  = " + sh(mzChain1()) + " recip=" + (1 / mzChain1()));
print("  chain2  = " + sh(mzChain2()) + " recip=" + (1 / mzChain2()));
print("  chain3  = " + sh(mzChain3()) + " recip=" + (1 / mzChain3()));
print("  chain4  = " + sh(mzChain4()) + " recip=" + (1 / mzChain4()));

print("== case 2: the algebraic identities, applied to values that are not proven int32");
var CALLS = 0;
var VO = {
  valueOf: function () { CALLS++; return 7; },
  toString: function () { CALLS++; return "s7"; }
};
var VAL = [
  ["0", 0], ["-0", -0], ["1", 1], ["-1", -1], ["2.7", 2.7], ["-2.7", -2.7],
  ["NaN", NaN], ["Inf", Infinity], ["-Inf", -Infinity],
  ["MAX", 2147483647], ["MAX+1", 2147483648], ["MIN", -2147483648],
  ["MIN-1", -2147483649], ["2^32", 4294967296], ["1e21", 1e21],
  ["'1'", "1"], ["'10'", "10"], ["''", ""], ["' '", " "],
  ["true", true], ["false", false], ["null", null], ["undef", undefined],
  ["1n", 1n], ["-1n", -1n], ["[]", []], ["[1]", [1]], ["{}", {}],
  ["valueOf", VO]
];
function oAdd0(x)   { var v = x; return v + 0; }
function oAddL0(x)  { var v = x; return 0 + v; }
function oSub0(x)   { var v = x; return v - 0; }
function oSubL0(x)  { var v = x; return 0 - v; }
function oMul1(x)   { var v = x; return v * 1; }
function oMulL1(x)  { var v = x; return 1 * v; }
function oAndNeg(x) { var v = x; return v & -1; }
function oOr0(x)    { var v = x; return v | 0; }
function oXor0(x)   { var v = x; return v ^ 0; }
function oShl0(x)   { var v = x; return v << 0; }
function oShr0(x)   { var v = x; return v >> 0; }
function oLnot2(x)  { var v = x; return !!v; }
function oNeg(x)    { var v = x; return -v; }
function oBnot(x)   { var v = x; return ~v; }
function oAddSelf(x){ var v = x; return v + v; }
function oMulSelf(x){ var v = x; return v * v; }
function oDiv1(x)   { var v = x; return v / 1; }
function oMod1(x)   { var v = x; return v % 1; }
var OPS = [
  ["x+0", oAdd0], ["0+x", oAddL0], ["x-0", oSub0], ["0-x", oSubL0],
  ["x*1", oMul1], ["1*x", oMulL1], ["x&-1", oAndNeg], ["x|0", oOr0],
  ["x^0", oXor0], ["x<<0", oShl0], ["x>>0", oShr0], ["!!x", oLnot2],
  ["-x", oNeg], ["~x", oBnot], ["x+x", oAddSelf], ["x*x", oMulSelf],
  ["x/1", oDiv1], ["x%1", oMod1]
];
for (vi = 0; vi < VAL.length; vi++) {
  var line = "  " + VAL[vi][0] + ":";
  for (oi = 0; oi < OPS.length; oi++) {
    var before = CALLS;
    try {
      var res = OPS[oi][1](VAL[vi][1]);
      line += " " + OPS[oi][0] + "=" + sh(res) + "/" + tp(res).charAt(0);
    } catch (e) {
      line += " " + OPS[oi][0] + "!" + e.name.charAt(0);
    }
    if (CALLS !== before) line += "(c" + (CALLS - before) + ")";
  }
  print(line);
}
print("  valueOf calls total = " + CALLS);

print("== case 3: constant branches, and what must not run in the dead arm");
function note(log, s) { log.push(s); return s; }
function boomLocal(log) { log.push("boom"); throw new Error("the dead arm ran"); }
function g1() { var log = [], on = 1 === 1; if (on) note(log, "on"); else boomLocal(log); return log.join("|"); }
function g2() { var log = [], on = 1 === 2; if (on) boomLocal(log); else note(log, "off"); return log.join("|"); }
function g3() { var log = [], n = 1 - 1; if (n) note(log, "truthy"); else note(log, "falsy"); return log.join("|"); }
function g4() { var log = [], s = ""; var n = s.length; if (n) boomLocal(log); else note(log, "empty"); return log.join("|"); }
safe("  g1", g1);
safe("  g2", g2);
safe("  g3", g3);
safe("  g4", g4);
/* The spec's own example: the unreachable assignment must not poison `mode`. */
function unseen() { throw new Error("unreachable"); }
function g5() {
  var log = [], enabled = true, mode;
  if (enabled) mode = 2; else mode = unseen();
  if (mode === 2) log.push("live"); else log.push("dead");
  return log.join("|");
}
safe("  g5", g5);
function g6() { var log = [], enabled = true, mode = 1; if (enabled) mode = 2; else mode = 3; return log.concat(mode).join("|"); }
function g7() { var log = [], mode = 1, c = false; if (c) mode = 2; return log.concat(mode).join("|"); }
function g8() { var log = [], mode = 1, c = true; if (c) mode = 2; return log.concat(mode).join("|"); }
function g9() { var log = [], mode = 1, c = 2; while (c > 0) { if (c === 2) log.push("a"); if (c === 1) log.push("b"); c--; } return log.concat(mode).join("|"); }
/* A condition that is constant only after both operands have been propagated. */
function g10() { var log = [], a = 2, b = 3, c = a < b; if (c) log.push("lt"); else log.push("ge"); return log.join("|"); }
function g11() { var log = [], a = 2, b = 3, c = a > b; if (c) log.push("gt"); else log.push("le"); return log.join("|"); }
function g12() { var log = [], a = null, c = a == null; if (c) log.push("nullish"); else log.push("no"); return log.join("|"); }
function g13() { var log = [], a = 1, b = 1, c = a === b; if (c) log.push("eq"); else log.push("ne"); return log.join("|"); }
var G = [g6, g7, g8, g9, g10, g11, g12, g13];
for (oi = 0; oi < G.length; oi++) safe("  g" + (oi + 6), G[oi]);

print("== case 4: joins -- a local constant on one path and not on another");
function j1(c)  { var v; if (c) v = 1; else v = 2; return v; }
function j2(c)  { var v = 1; if (c) v = 2; return v; }
function j3(c)  { var v = 1; if (c) v = 9; return v + 0; }
function j4(c)  { var v = "1"; if (c) v = "9"; return v + 0; }
function j5(c)  { var v = 1; while (c > 0) { v = v * 2; c--; } return v; }
function j6(c)  { var v = 1; for (var i = 0; i < c; i++) v = v + 1; return v; }
function j7(c)  { var v = 3; switch (c) { case 0: v = 4; break; case 1: v = 5; case 2: v = 6; break; } return v; }
function j8(c)  { var v = 2; try { if (c) throw 1; v = 3; } catch (e) { v = 4; } return v; }
function j9(c)  { var v = 1; do { v *= 3; c--; } while (c > 0); return v; }
function j10(c) { var v = 1; l: { if (c) { v = 2; break l; } v = 3; } return v; }
function j11(c) { var v = 1; var f = function () { v = 5; }; if (c) f(); return v; }
function j12(c) { var v = 1; if (c) { v = 2; } if (!c) { v = 3; } return v; }
function j13(c) { var v; switch (c) { default: v = "d"; } return v; }
function j14(c) { var v = 0; if (c > 0) { v = "s"; } else if (c === 0) { v = null; } else { v = undefined; } return v; }
function j15(c) { var v = 1; for (;;) { if (c > 0) { v = 2; break; } break; } return v; }
function j16(c) { var v = 1, w = 2; if (c) { v = w; } else { w = v; } return v * 10 + w; }
function j17(c) { var v = 1; if (c) { try { v = 2; } finally { v = 3; } } return v; }
function j18(c) { var v = 1; try { v = 2; } catch (e) { v = 3; } finally { if (c) v = 4; } return v; }
var J = [j1, j2, j3, j4, j5, j6, j7, j8, j9, j10, j11, j12, j13, j14, j15, j16, j17, j18];
var JC = [-1, 0, 1, 2, 3];
for (vi = 0; vi < J.length; vi++) {
  var jrow = "  j" + (vi + 1) + ":";
  for (oi = 0; oi < JC.length; oi++) {
    jrow += " " + JC[oi] + "->";
    try { jrow += sh(J[vi](JC[oi])); } catch (e) { jrow += "!" + e.name; }
  }
  print(jrow);
}

print("== case 5: dead stores, live stores, set_loc and the stack");
var SIDE = 0;
function touch(x) { SIDE++; return x; }
function d1(x)  { var a = x * 3; return x + 1; }
function d2(x)  { var a = 1; a = 2; return a; }
function d3(x)  { var a = 1; a = 2; return x; }
function d4(x)  { var a = 1; a += x; return a; }
function d5(x)  { var a = x; var b = a++; return a * 100 + b; }
function d6(x)  { var a = x; var b = ++a; return a * 100 + b; }
function d7(x)  { var a; var b = (a = 3); return a * 10 + b; }
function d8(x)  { var a = touch(x); return SIDE; }
function d9(x)  { var a = x; a = a; return a; }
function d10(x) { var i = 0; var arr = [10, 20, 30]; var r = arr[i++]; return r * 100 + i; }
function d11(x) { var o = { k: 1 }; var a = o.k; o.k = 2; return a + o.k; }
function d12(x) { var a = x; { var b = a; a = 5; return b * 100 + a; } }
function d13(x) { var a = x; var b = a; a = 7; return b + a; }
function d14(x) { var a = x, b = x; b = b + 1; a = b; return a; }
function d15(x) { var a = 0; for (var i = 0; i < 3; i++) { a = a + i; } return a; }
function d16(x) { var a = 0; for (var i = 0; i < 3; i++) { a = i; } return a; }
function d17(x) { var a = x; var r = (a = a * 2) + a; return r; }
function d18(x) { var unused = x * 2; var alsoUnused = x + 1; return x; }
function d19(x) { var a = 1; if (x) { a = 2; } return 1; }
function d20(x) { var a = 1; var b = a; return b; }
function d21(x) { var a = x; var b = a; var c = b; return c; }
function d22(x) { var a = x; a -= 0; return a; }
function d23(x) { var a = x; var b = a; a = a + 1; return b * 100 + a; }
function d24(x) { var a = x, b = 2; var c = a * b; b = 3; return c * 100 + a * b; }
var D = [d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12, d13, d14, d15, d16,
         d17, d18, d19, d20, d21, d22, d23, d24];
for (vi = 0; vi < D.length; vi++) {
  SIDE = 0;
  var drow = "  d" + (vi + 1) + ":";
  for (oi = 0; oi < 3; oi++) {
    drow += " " + oi + "->";
    try { drow += sh(D[vi](oi)); } catch (e) { drow += "!" + e.name; }
  }
  print(drow + " side=" + SIDE);
}

print("== case 6: the three routes by which a local escapes the body");
function c1() { var a = 1; var f = function () { return a; }; a = 2; return f(); }
function c2() { var a = 1; var f = function () { a = 5; }; f(); return a; }
function c3() { var a = 1; function g() { return a; } a = 3; return g(); }
function c4() { var a = 1; eval("a = 7"); return a; }
function c5() { var a = 1, s = "a"; eval("a = 8"); return a + s.length; }
function c6() { var a = 1; (0, eval)("var a = 9"); return a; }
function c7(a) { var b = a; arguments[0] = 9; return b * 10 + a; }
function c8(a) { var b = a; return function () { return b; }(); }
function c9() { var a = 1; var f = function () { a = a + 1; return a; }; return f() + "," + f() + "," + a; }
function c10() { var a = 1; return [a, function () { return a; }][1]() + a; }
function c11(a) { var b = a; var f = function () { b = b + 1; }; f(); f(); return b; }
function c12() { var a = 1; var f = function () { return a; }; a = 2; var g = function () { a = 3; }; g(); return a + f(); }
function c13() { var o = {}; var a = 1; o.get = function () { return a; }; a = 4; return o.get(); }
var C = [c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12, c13];
for (vi = 0; vi < C.length; vi++) {
  var crow = "  c" + (vi + 1) + ":";
  for (oi = 0; oi < 3; oi++) {
    crow += " " + oi + "->";
    try { crow += sh(C[vi](oi)); } catch (e) { crow += "!" + e.name; }
  }
  print(crow);
}

print("== case 7: control flow the simplifier may rewrite, and what it must not");
function f1(n)  { if (n) return "a"; return "b"; }
function f2(n)  { var r; if (n > 0) r = "p"; else if (n < 0) r = "n"; else r = "z"; return r; }
function f3(n)  { switch (n) { case 0: return "zero"; case 1: case 2: return "small"; default: return "big"; } }
/* the `i > 5` guard is for the undefined argument: `0 >= undefined` is
   false and the loop would not end.  For every other argument it is
   unreachable, because the table below never passes a value above 2. */
function f4(n)  { var s = 0, i = 0; while (true) { if (i >= n || i > 5) break; i++; if (i & 1) continue; s += i; } return s; }
function f5()   {
  var out = "";
  outer: for (var i = 0; i < 3; i++) {
    for (var k = 0; k < 3; k++) {
      if (k === 1 && i === 1) continue outer;
      if (i === 2) break outer;
      out += "" + i + k + " ";
    }
  }
  return out;
}
function f6()   { var out = ""; try { out += "t"; throw 1; } catch (e) { out += "c" + e; } finally { out += "f"; } return out; }
function f7()   { try { return "r"; } finally { } }
function f8()   { var out = ""; try { try { throw "x"; } finally { out += "inner"; } } catch (e) { out += "caught" + e; } return out; }
function f9(n)  { var out = ""; for (var i = 0; i < n; i++) { if (i === 2) return out + "ret"; out += i; } return out + "end"; }
function f10(n) { var out = ""; do { out += "d"; n--; } while (n > 0); return out; }
function f11(n) { var out = ""; for (var k of [1, 2, 3]) { if (k > n) break; out += k; } return out; }
function f12()  { try { return "t"; } finally { return "f"; } }
function f13()  { var log = []; try { log.push("a"); return log.join(","); } finally { log.push("b"); } }
function f14(n) { var r = 0; if (n) { } else { } if (n) r++; return r; }
function f15(n) { var r = 0; { } if (n) { r = 1; } return r; }
function f16(n) { var out = ""; for (var i = 0; i < n; i++) { switch (i) { case 0: out += "0"; break; case 1: continue; default: out += "d"; } out += "-"; } return out; }
function f17(n) { var out = ""; a: for (var i = 0; i < 3; i++) { b: for (var k = 0; k < 3; k++) { if (k === 2) continue b; if (i === 1 && k === 1) break a; out += "" + i + k; } } return out; }
function f18(n) { var out = ""; for (var i = 0; i < n; i++) { if (i === 0) { out += "z"; } else if (i === 1) { out += "o"; } else { out += "m"; } } return out + "|"; }
function f19(n) { var out = "", i = 0; while (i < n) { i++; if (i === 2) return out + "R"; out += i; } return out + "E"; }
function f20(n) { var out = ""; switch (n) { default: out += "d"; case 0: out += "z"; break; case 1: out += "o"; } return out; }
function f21(n) { var out = ""; L: { if (n) { out += "a"; break L; } out += "b"; } return out; }
var F = [f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16,
         f17, f18, f19, f20, f21];
var FARG = [undefined, 0, 1, 2];
for (vi = 0; vi < F.length; vi++) {
  var frow = "  f" + (vi + 1) + ":";
  for (oi = 0; oi < FARG.length; oi++) {
    frow += " " + sh(FARG[oi]) + "->";
    try { frow += sh(F[vi](FARG[oi])); } catch (e) { frow += "!" + e.name; }
  }
  print(frow);
}
function* gen1(n) { for (var i = 0; i < n; i++) { if (i === 1) yield "one"; else yield i; } return "done"; }
function* gen2(n) { var a = 1; try { while (n-- > 0) { a = a * 2; yield a; } } finally { yield "fin"; } return a; }
function genDrive(g, n) {
  var it = g(n), seq = [], r = it.next();
  while (!r.done) { seq.push(r.value); r = it.next(); }
  return seq.join(",") + " done=" + sh(r.value);
}
safe("  gen1", function () { return genDrive(gen1, 3); });
safe("  gen2", function () { return genDrive(gen2, 3); });

print("== case 8: wide local forms -- more than 256 locals");
function wide(a) {
  var v0 = 0, v1 = 1, v2 = 2, v3 = 3, v4 = 4, v5 = 5;
  var v6 = 6, v7 = 7, v8 = 8, v9 = 9, v10 = 10, v11 = 11;
  var v12 = 12, v13 = 13, v14 = 14, v15 = 15, v16 = 16, v17 = 17;
  var v18 = 18, v19 = 19, v20 = 20, v21 = 21, v22 = 22, v23 = 23;
  var v24 = 24, v25 = 25, v26 = 26, v27 = 27, v28 = 28, v29 = 29;
  var v30 = 30, v31 = 31, v32 = 32, v33 = 33, v34 = 34, v35 = 35;
  var v36 = 36, v37 = 37, v38 = 38, v39 = 39, v40 = 40, v41 = 41;
  var v42 = 42, v43 = 43, v44 = 44, v45 = 45, v46 = 46, v47 = 47;
  var v48 = 48, v49 = 49, v50 = 50, v51 = 51, v52 = 52, v53 = 53;
  var v54 = 54, v55 = 55, v56 = 56, v57 = 57, v58 = 58, v59 = 59;
  var v60 = 60, v61 = 61, v62 = 62, v63 = 63, v64 = 64, v65 = 65;
  var v66 = 66, v67 = 67, v68 = 68, v69 = 69, v70 = 70, v71 = 71;
  var v72 = 72, v73 = 73, v74 = 74, v75 = 75, v76 = 76, v77 = 77;
  var v78 = 78, v79 = 79, v80 = 80, v81 = 81, v82 = 82, v83 = 83;
  var v84 = 84, v85 = 85, v86 = 86, v87 = 87, v88 = 88, v89 = 89;
  var v90 = 90, v91 = 91, v92 = 92, v93 = 93, v94 = 94, v95 = 95;
  var v96 = 96, v97 = 97, v98 = 98, v99 = 99, v100 = 100, v101 = 101;
  var v102 = 102, v103 = 103, v104 = 104, v105 = 105, v106 = 106, v107 = 107;
  var v108 = 108, v109 = 109, v110 = 110, v111 = 111, v112 = 112, v113 = 113;
  var v114 = 114, v115 = 115, v116 = 116, v117 = 117, v118 = 118, v119 = 119;
  var v120 = 120, v121 = 121, v122 = 122, v123 = 123, v124 = 124, v125 = 125;
  var v126 = 126, v127 = 127, v128 = 128, v129 = 129, v130 = 130, v131 = 131;
  var v132 = 132, v133 = 133, v134 = 134, v135 = 135, v136 = 136, v137 = 137;
  var v138 = 138, v139 = 139, v140 = 140, v141 = 141, v142 = 142, v143 = 143;
  var v144 = 144, v145 = 145, v146 = 146, v147 = 147, v148 = 148, v149 = 149;
  var v150 = 150, v151 = 151, v152 = 152, v153 = 153, v154 = 154, v155 = 155;
  var v156 = 156, v157 = 157, v158 = 158, v159 = 159, v160 = 160, v161 = 161;
  var v162 = 162, v163 = 163, v164 = 164, v165 = 165, v166 = 166, v167 = 167;
  var v168 = 168, v169 = 169, v170 = 170, v171 = 171, v172 = 172, v173 = 173;
  var v174 = 174, v175 = 175, v176 = 176, v177 = 177, v178 = 178, v179 = 179;
  var v180 = 180, v181 = 181, v182 = 182, v183 = 183, v184 = 184, v185 = 185;
  var v186 = 186, v187 = 187, v188 = 188, v189 = 189, v190 = 190, v191 = 191;
  var v192 = 192, v193 = 193, v194 = 194, v195 = 195, v196 = 196, v197 = 197;
  var v198 = 198, v199 = 199, v200 = 200, v201 = 201, v202 = 202, v203 = 203;
  var v204 = 204, v205 = 205, v206 = 206, v207 = 207, v208 = 208, v209 = 209;
  var v210 = 210, v211 = 211, v212 = 212, v213 = 213, v214 = 214, v215 = 215;
  var v216 = 216, v217 = 217, v218 = 218, v219 = 219, v220 = 220, v221 = 221;
  var v222 = 222, v223 = 223, v224 = 224, v225 = 225, v226 = 226, v227 = 227;
  var v228 = 228, v229 = 229, v230 = 230, v231 = 231, v232 = 232, v233 = 233;
  var v234 = 234, v235 = 235, v236 = 236, v237 = 237, v238 = 238, v239 = 239;
  var v240 = 240, v241 = 241, v242 = 242, v243 = 243, v244 = 244, v245 = 245;
  var v246 = 246, v247 = 247, v248 = 248, v249 = 249, v250 = 250, v251 = 251;
  var v252 = 252, v253 = 253, v254 = 254, v255 = 255, v256 = 256, v257 = 257;
  var v258 = 258, v259 = 259, v260 = 260, v261 = 261, v262 = 262, v263 = 263;
  var v264 = 264, v265 = 265, v266 = 266, v267 = 267, v268 = 268, v269 = 269;
  var v270 = 270, v271 = 271, v272 = 272, v273 = 273, v274 = 274, v275 = 275;
  var v276 = 276, v277 = 277, v278 = 278, v279 = 279, v280 = 280, v281 = 281;
  var v282 = 282, v283 = 283, v284 = 284, v285 = 285, v286 = 286, v287 = 287;
  var v288 = 288, v289 = 289, v290 = 290, v291 = 291, v292 = 292, v293 = 293;
  var v294 = 294, v295 = 295, v296 = 296, v297 = 297, v298 = 298, v299 = 299;
  var out = a;
  out += v0 + v3 + v4 + v5 + v100 + v255 + v256 + v299;
  if (a > 0) { v10 = v10 + 1; out += v10; } else { v257 = v257 * 2; out += v257; }
  while (a > 0) { v255 = v255 - 1; out += v255; a--; }
  return out;
}
for (oi = 0; oi < 3; oi++) { try { print("  wide(" + oi + ") = " + sh(wide(oi))); } catch (e) { print("  wide(" + oi + ") threw " + e.name); } }

print("== case 9: what the constant folder may fold, and the traps beside it");
var K = [
  ["2+3",          function () { var a = 2, b = 3; return a + b; }],
  ["2-3",          function () { var a = 2, b = 3; return a - b; }],
  ["2*3",          function () { var a = 2, b = 3; return a * b; }],
  ["MAX+1",        function () { var a = 2147483647, b = 1; return a + b; }],
  ["MIN-1",        function () { var a = -2147483648, b = 1; return a - b; }],
  ["MAX+MAX",      function () { var a = 2147483647, b = 2147483647; return a + b; }],
  ["MIN*-1",       function () { var a = -2147483648, b = -1; return a * b; }],
  ["65536*65536",  function () { var a = 65536, b = 65536; return a * b; }],
  ["7/0",          function () { var a = 7, b = 0; return a / b; }],
  ["-7/0",         function () { var a = -7, b = 0; return a / b; }],
  ["0/0",          function () { var a = 0, b = 0; return a / b; }],
  ["7%0",          function () { var a = 7, b = 0; return a % b; }],
  ["5/2",          function () { var a = 5, b = 2; return a / b; }],
  ["5%2",          function () { var a = 5, b = 2; return a % b; }],
  ["-7/2",         function () { var a = -7, b = 2; return a / b; }],
  ["-7%2",         function () { var a = -7, b = 2; return a % b; }],
  ["7%-2",         function () { var a = 7, b = -2; return a % b; }],
  ["1.5*2",        function () { var a = 1.5, b = 2; return a * b; }],
  ["-0+0",         function () { var a = -0, b = 0; return a + b; }],
  ["0.1+0.2",      function () { var a = 0.1, b = 0.2; return a + b; }],
  ["1e308*10",     function () { var a = 1e308, b = 10; return a * b; }],
  ["-1>>1",        function () { var a = -1, b = 1; return a >> b; }],
  ["-1>>>1",       function () { var a = -1, b = 1; return a >>> b; }],
  ["1<<31",        function () { var a = 1, b = 31; return a << b; }],
  ["1<<32",        function () { var a = 1, b = 32; return a << b; }],
  ["-5>>32",       function () { var a = -5, b = 32; return a >> b; }],
  ["3000000000|0", function () { var a = 3000000000; return a | 0; }],
  ["2.7|0",        function () { var a = 2.7; return a | 0; }],
  ["-2.7|0",       function () { var a = -2.7; return a | 0; }],
  ["1e21|0",       function () { var a = 1e21; return a | 0; }],
  ["NaN|0",        function () { var a = NaN; return a | 0; }],
  ["Inf|0",        function () { var a = Infinity; return a | 0; }],
  ["'1'+1",        function () { var a = "1", b = 1; return a + b; }],
  ["'1'-1",        function () { var a = "1", b = 1; return a - b; }],
  ["'a'<1",        function () { var a = "a", b = 1; return a < b; }],
  ["null<1",       function () { var a = null, b = 1; return a < b; }],
  ["undef<1",      function () { var a = undefined, b = 1; return a < b; }],
  ["true+true",    function () { var a = true, b = true; return a + b; }],
  ["null+1",       function () { var a = null, b = 1; return a + b; }],
  ["undef+1",      function () { var a = undefined, b = 1; return a + b; }],
  ["1n+1",         function () { var a = 1n, b = 1; return a + b; }],
  ["1n*2n",        function () { var a = 1n, b = 2n; return a * b; }],
  ["1==1",         function () { var a = 1, b = 1; return a == b; }],
  ["'1'==1",       function () { var a = "1", b = 1; return a == b; }],
  ["1===1",        function () { var a = 1, b = 1; return a === b; }],
  ["'1'===1",      function () { var a = "1", b = 1; return a === b; }],
  ["null==undef",  function () { var a = null, b = undefined; return a == b; }],
  ["null===undef", function () { var a = null, b = undefined; return a === b; }],
  ["NaN==NaN",     function () { var a = NaN, b = NaN; return a == b; }],
  ["'10'<9",       function () { var a = "10", b = 9; return a < b; }],
  ["'10'<'9'",     function () { var a = "10", b = "9"; return a < b; }]
];
for (vi = 0; vi < K.length; vi++) safeT("  " + K[vi][0], K[vi][1]);
/* The same values in BRANCH position: a folded comparison deletes one of
   these arms, so an arm that should have been taken has to stay visible. */
function b1()  { var a = 2, b = 3; if (a < b) return "T"; return "F"; }
function b2()  { var a = 2, b = 3; if (a > b) return "T"; return "F"; }
function b3()  { var a = "10", b = 9; if (a < b) return "T"; return "F"; }
function b4()  { var a = "10", b = "9"; if (a < b) return "T"; return "F"; }
function b5()  { var a = null, b = 1; if (a < b) return "T"; return "F"; }
function b6()  { var a = undefined, b = 1; if (a < b) return "T"; return "F"; }
function b7()  { var a = NaN; if (a < 1) return "T"; return "F"; }
function b8()  { var a = "1", b = 1; if (a == b) return "T"; return "F"; }
function b9()  { var a = "1", b = 1; if (a === b) return "T"; return "F"; }
function b10() { var a = null, b = undefined; if (a == b) return "T"; return "F"; }
function b11() { var a = null, b = undefined; if (a === b) return "T"; return "F"; }
function b12() { var a = 2147483647, b = 1; if (a + b > 0) return "T"; return "F"; }
function b13() { var a = -2147483648, b = 1; if (a - b < 0) return "T"; return "F"; }
function b14() { var a = -0, b = 0; if (a < b) return "T"; return "F"; }
function b15() { var a = -0, b = 0; if (a === b) return "T"; return "F"; }
function b16() { var a = [1, 2], b = 3; if (a < b) return "T"; return "F"; }
function b17() { var a = 1n, b = 2n; if (a < b) return "T"; return "F"; }
function b18() { var a = 1n, b = 2; if (a < b) return "T"; return "F"; }
var B = [b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11, b12, b13, b14, b15, b16, b17, b18];
for (vi = 0; vi < B.length; vi++) safe("  b" + (vi + 1), B[vi]);

print("== case 10: stack shapes -- the depth the next block assumes");
function s1(a, b, c) { return a * (b + c) - (a - b) * c; }
function s2(a, b) { var t = a; a = b; b = t; return a * 10 + b; }
function s3(a) { var arr = [a, a + 1, a + 2]; return arr[0] + arr[1] * 10 + arr[2] * 100; }
function s4(a) { var o = { x: a, y: a + 1 }; return o.y * 10 + o.x; }
function s5(a, b) { return (a, b); }
function s6(a) { var f = function (x) { return x + a; }; return f(1) + f(2); }
function s7(a) { var r = 0; for (var i = 0; i < 3; i++) r += (a + i) * (i + 1); return r; }
function s8(a) { var m = [[1, 2], [3, 4]]; return m[0][1] + m[1][0] * 10 + a; }
function s9(a) { var s = ""; s += a; s += a + 1; return s; }
function s10(arr) { var i = 0, j = 0; return arr[i++] + arr[j++] * 10; }
function s11(a, b) { return { p: a, q: b, r: a + b }; }
function s12(a) { var r = []; r.push(a); r.push(a * 2); return r.join(","); }
function s13(a) { var x = a, y = a + 1, z = a + 2; return z * 100 + y * 10 + x; }
function s14(a) { var o = {}; o["k" + a] = a; return o["k" + a]; }
function s15(a) { return a ? (a > 1 ? "big" : "one") : "none"; }
function s16(a) { var f = function () { return arguments.length; }; return f(a, a, a); }
function s17(a) { var s = 0; for (var i = 0; i < 3; i++) { for (var j = 0; j < 2; j++) { s += a * i + j; } } return s; }
function s18(a) { var x = a; return void (x = 9), x; }
function s19(a) { var x = a; return typeof x + "," + typeof (x = "s"); }
function s20(a) { var arr = [1, 2, 3]; var i = 1; arr[i] = arr[i - 1] + arr[i + 1]; return arr.join(","); }
var S = [s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15, s16, s17, s18, s19, s20];
for (vi = 0; vi < S.length; vi++) {
  var srow = "  s" + (vi + 1) + ":";
  var argSets = vi === 9 ? [[10, 20, 30]] : [[0], [1], [2], [3]];
  for (oi = 0; oi < argSets.length; oi++) {
    srow += " ";
    try { srow += sh(S[vi].apply(null, argSets[oi])); } catch (e) { srow += "!" + e.name; }
  }
  print(srow);
}
(function () { print("  s1(1,2,3) = " + sh(s1(1, 2, 3)) + " s2(1,2) = " + sh(s2(1, 2)) + " s11 = " + sh(s11(1, 2))); })();
