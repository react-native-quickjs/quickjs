/* Adversarial corpus for the optimizer's value ranges and its branch
   narrowing -- the pass behind `qjsc -O`.

   The pass tracks an int32 interval per value and narrows it along the edge a
   test goes by: `if (i < 4096)` taken means i <= 4095 on the taken side. What
   the interval buys is the right to call a value an exact int32, and that
   right is what every case here attacks, because a wrong int32 claim turns
   `x + 0` into `x`, `x | 0` into `x`, `x * 0` into `+0`, and a comparison of
   two Numbers into something that is not the comparison the source wrote.

   Every line is compared byte for byte against node on all three arms
   (source, plain bytecode, and `-O` bytecode), so what each case
   discriminates with is node's answer, not an expectation written here.

   ## What each case is for

     case                          discriminates
     ----------------------------  ----------------------------------------
     1  counters under every test  the narrowed value observed inside the
                                   branch and after the loop, at bounds where
                                   the claim changes the answer: i+i leaving
                                   int32, i*i leaving int32, 1/i at zero
                                   (which sees -0), i/2 (a double division
                                   that must not become a shift), and the
                                   value's own type.
     2  bounds that are not int32  a bound the pass must not narrow by: a
                                   comparison against a constant outside
                                   int32, against a fraction, against a
                                   string, against NaN (false either way),
                                   against -0, and against a value that is
                                   not a Number at all.
     3  a local written between   i < (i = j): the comparison is about the
        the load and the test      value the load read, and the local no
                                   longer holds it at the test.  The bound
                                   would be about the wrong value, and the
                                   bodies here make that observable with
                                   i + i | 0 near the top of int32.
     4  masks                     x & c bounds x below the mask, and x & c is
                                   an exact int32 -- but only for the operand
                                   shapes where the coercion is an int32
                                   truncation: doubles, NaN, Infinity, -0,
                                   strings, objects with valueOf, and
                                   BigInt, which must still throw.
     5  arithmetic on a range     what the interval licenses: x & 255 is in
                                   [0, 255], so +1, -300 and (*) stay int32
                                   -- and what it does not: a counter that
                                   leaves int32 must be a double afterwards.
     6  narrowing across edges    a bound carried through a loop back edge and
                                   through a catch edge, and a bound that
                                   another path into the same block does not
                                   establish, which must therefore be lost.

   Strict mode once at the top, as in the sibling corpus: the corpus runs as a
   script in qjs and as a module under node, and a module is always strict.
*/

"use strict";

/* sh() makes the values a wrong int32 claim destroys visible: -0, NaN and the
   infinities all print the same as something they are not. */
function sh(v) {
  var t = typeof v;
  if (t === "number") {
    if (v === 0 && 1 / v < 0) return "-0";
    if (v === v) return String(v);
    return "NaN";
  }
  if (t === "bigint") return String(v) + "n";
  if (t === "string") return JSON.stringify(v);
  return String(v);
}
function tp(v) { return typeof v; }
function safe(label, f) {
  try { var r = f(); print(label + " = " + sh(r) + " :" + tp(r)); }
  catch (e) { print(label + " threw " + e.name); }
}

print("== case 1: counters under every test");
/* The counter is printed AND used, so the narrowed value is observed rather
   than only controlled: a narrowing that is wrong about i here is wrong about
   the number node prints. */
var LOOPS = [
  ["while i < 6",      "var i = 0, s = []; while (i < 6) { s.push(i + \"|\" + sh((i + 0)) + \"|\" + tp(i)); i++; } return s.join(\",\");"],
  ["while i <= 6",     "var i = 0, s = []; while (i <= 6) { s.push(sh(i)); i++; } return s.join(\",\");"],
  ["while i > -6",     "var i = 0, s = []; while (i > -6) { s.push(sh(i)); i--; } return s.join(\",\");"],
  ["while i >= -6",    "var i = 0, s = []; while (i >= -6) { s.push(sh(i)); i--; } return s.join(\",\");"],
  ["while 6 > i",      "var i = 0, s = []; while (6 > i) { s.push(sh(i)); i++; } return s.join(\",\");"],
  ["while 6 >= i",     "var i = 0, s = []; while (6 >= i) { s.push(sh(i)); i++; } return s.join(\",\");"],
  ["while -6 < i",     "var i = 0, s = []; while (-6 < i) { s.push(sh(i)); i--; } return s.join(\",\");"],
  ["while -6 <= i",    "var i = 0, s = []; while (-6 <= i) { s.push(sh(i)); i--; } return s.join(\",\");"],
  /* i / 2 is a double division: 5/2 is 2.5, and a value wrongly called an
     int32 is a value a shift could be invented for. */
  ["i / 2 in body",    "var i = 0, s = []; while (i < 6) { s.push(sh(i / 2)); i++; } return s.join(\",\");"],
  ["1 / i at zero",    "var i = 0, s = []; while (i < 3) { s.push(sh(1 / (i - 0))); i++; } return s.join(\",\");"],
  /* i + i leaves int32 as the counter passes 2^30: 2^30 + 2^30 is a double,
     and `(i + i) | 0` is not `i + i` when it is. */
  ["i + i | 0",        "var i = 1073741820, s = []; while (i < 1073741830) { s.push(sh((i + i) | 0) + \"/\" + sh(i + i)); i++; } return s.join(\",\");"],
  ["i * i | 0",        "var i = 46330, s = []; while (i < 46340) { s.push(sh(i * i) + \"/\" + sh((i * i) | 0)); i++; } return s.join(\",\");"],
  /* The bound on the closing side: i >= -6 narrows from below, and the value
     is used multiplied. */
  ["negative bound",   "var i = -4, s = []; while (i >= -6) { s.push(sh(i * 2) + \"/\" + sh(i - 0)); i--; } return s.join(\",\");"],
  ["bound at min",     "var i = -2147483647, s = []; while (i >= -2147483648) { s.push(sh(i + 0) + \"/\" + sh(i + i)); i--; } return s.join(\",\");"],
  ["bound at max",     "var i = 2147483640, s = []; while (i <= 2147483647) { s.push(sh(i + 0) + \"/\" + sh(i + i)); i++; } return s.join(\",\");"],
  /* A bound exactly at the edge of int32 must stop narrowing rather than
     wrap the bound into the interval. */
  ["bound one past",   "var i = 2147483640; var n = 0; while (i < 2147483648) { n++; i++; } return n;"],
  ["bound below min",  "var i = -2147483648; var n = 0; while (i > -2147483649) { n++; i--; } return n;"],
];
for (var l = 0; l < LOOPS.length; l++) {
  /* The body is built as a function so that no case is folded by the parser
     before the optimizer sees it.  sh() and tp() are passed in rather than
     reached through the global object: this file is a script in qjs and a
     module under node, and a module's bindings are not global. */
  var nm = LOOPS[l][0];
  var fn = new Function("sh", "tp", LOOPS[l][1]);
  (function (label, f) {
    safe("  " + label, function () { return f(sh, tp); });
  })(nm, fn);
}

print("== case 2: bounds that are not int32");
var NOTI = [
  ["i < 2147483650",  function () { var i = 2147483647; if (i < 2147483650) return i + i; return -1; }],
  ["i < 1.5",         function () { var i = 1.5; var t = 0; while (i < 3.5) { t = t + sh(i); i = i + 0.5; } return t; }],
  ["i < \"5\"",        function () { var i = 3; var s = []; while (i < "5") { s.push(i); i++; } return s.join(","); }],
  ["i <= \"4x\"",      function () { var i = 3; if (i <= "4x") return "yes"; return "no"; }],
  ["i < NaN",         function () { var i = 0; if (i < NaN) return "taken"; return "not"; }],
  ["i >= NaN",        function () { var i = 0; if (i >= NaN) return "taken"; return "not"; }],
  ["i < -0",          function () { var i = -0; if (i < -0) return "taken"; return "not"; }],
  ["i <= -0",         function () { var i = -0; if (i <= -0) return 1 / i; return "not"; }],
  ["i < undefined",   function () { var i = 0; if (i < undefined) return "taken"; return "not"; }],
  ["i > null",        function () { var i = 0; if (i > null) return "taken"; return "not"; }],
  ["i < true",        function () { var i = 0; if (i < true) return "taken"; return "not"; }],
  ["i <= -1e10",      function () { var i = -2147483648; if (i <= -1e10) return "taken"; return i + i; }],
  ["bound via local", function () { var k = 2147483650; var i = 2147483647; if (i < k) return i + i; return -1; }],
  /* A comparison the pass cannot narrow by because the operand is not a
     Number at all, in both operand orders. */
  ["\"a\" < i",        function () { var i = 3; if ("a" < i) return "taken"; return "not"; }],
  ["{} < i",          function () { var i = 3; if ({ valueOf: function () { return 2; } } < i) return "taken"; return "not"; }],
];
for (var m = 0; m < NOTI.length; m++) safe("  " + NOTI[m][0], NOTI[m][1]);

print("== case 3: a local written between the load and the test");
/* Every one of these compares a value that was read before the local was
   written, so the local at the test is not what was compared.  Taking the
   bound from the comparison would put the wrong range on i, and i + i | 0
   near the top of int32 is where that shows. */
var WRITTEN = [
  ["i < (i = j)",       function (a) { var i = 1, j = a | 0; if (i < (i = j)) return (i + i) | 0; return "not"; }],
  ["i < (i = 30)",      function () { var i = 1; if (i < (i = 30)) return (i + i) | 0; return "not"; }],
  ["i <= (i = i + 1)",  function () { var i = 2147483646; if (i <= (i = i + 1)) return (i + i) | 0; return "not"; }],
  ["(i = j) > i",       function (a) { var i = 1, j = a | 0; i = j; if ((i - 1) > i) return (i + i) | 0; return i + 1; }],
  ["i > (i = -j)",      function (a) { var i = 5, j = a | 0; if (i > (i = -j)) return (i + i) | 0; return i + 1; }],
  ["i < (i = j) <= 9",  function (a) { var i = 1, j = a | 0; if (i < (i = j) && i <= 9) return (i + i) | 0; return "not"; }],
  ["i++ then i < 5",    function () { var i = 2147483646; var t = i++; if (t < 5) return (i + i) | 0; return i; }],
  ["i < 5 with i++",    function () { var i = 1; if (i++ < 5) return (i + i) | 0; return i; }],
];
for (var w = 0; w < WRITTEN.length; w++) {
  var wf = WRITTEN[w][1];
  safe("  " + WRITTEN[w][0] + " (j=7)", function (f) { return function () { return f(7); }; }(wf));
  safe("  " + WRITTEN[w][0] + " (j=-9)", function (f) { return function () { return f(-9); }; }(wf));
  safe("  " + WRITTEN[w][0] + " (j=2^31-1)", function (f) { return function () { return f(2147483647); }; }(wf));
}

print("== case 4: masks");
var MASKS = [
  ["x & 255",   "return (x & 255);"],
  ["x & 0",     "return (x & 0);"],
  ["x & 3",     "return (x & 3) + 1;"],
  ["x & 127",   "return 1 / (x & 127);"],
  ["(x&255)*1", "return (x & 255) * 1;"],
  ["x & -1",    "return x & -1;"],
  ["typeof(x&3)", "return typeof (x & 3);"],
  ["(x&3)===x", "return (x & 3) === x;"],
];
var XVALS = [
  ["7", "7"], ["0", "0"], ["-0", "-0"], ["-3", "-3"], ["1e10", "1e10"],
  ["2^31-1", "2147483647"], ["2^31", "2147483648"], ["-2^31", "-2147483648"],
  ["-2^31-1", "-2147483649"], ["NaN", "NaN"], ["Infinity", "Infinity"],
  ["-Infinity", "-Infinity"], ["0.5", "0.5"], ["-0.5", "-0.5"],
  ["\"12\"", "\"12\""], ["\"12x\"", "\"12x\""], ["\"\"", "\"\""],
  ["null", "null"], ["undefined", "undefined"], ["true", "true"],
  ["[7]", "[7]"], ["[7.9]", "[7.9]"],
  ["{valueOf:7}", "(function () { return { valueOf: function () { return 7; } }; })()"],
  ["2n", "(typeof BigInt === \"function\" ? 2n : 2)"],
  ["1n", "(typeof BigInt === \"function\" ? 1n : 1)"],
];
for (var i2 = 0; i2 < MASKS.length; i2++) {
  for (var j2 = 0; j2 < XVALS.length; j2++) {
    var body = "(function (x) { " + MASKS[i2][1] + " })";
    var f2 = new Function("x", MASKS[i2][1]);
    var v2 = new Function("return " + XVALS[j2][1])();
    (function (label, fn, val) {
      safe("  " + label, function () { return fn(val); });
    })(MASKS[i2][0] + " on " + XVALS[j2][0], f2, v2);
  }
}

print("== case 5: arithmetic on a range");
var RANGES = [
  ["(x&255)+1",        function (x) { return (x & 255) + 1; }],
  ["(x&255)-300",      function (x) { return (x & 255) - 300; }],
  ["(x&255)*(x&255)",  function (x) { return (x & 255) * (x & 255); }],
  ["(x&255)+(y&255)",  function (x, y) { return (x & 255) + (y & 255); }],
  ["((x&255)+1)|0",    function (x) { return ((x & 255) + 1) | 0; }],
  ["(x&255)@2^31",     function (x) { return (x & 255) * 8388608 * 256; }],
  ["(x&3)-(x&3)",      function (x) { return (x & 3) - (x & 3); }],
  ["1/((x&3)-(x&3))",  function (x) { return 1 / ((x & 3) - (x & 3)); }],
  ["(x&255)+0.5",      function (x) { return (x & 255) + 0.5; }],
];
var RVALS = ["0", "-1", "9", "2147483647", "-2147483648", "1e10", "\"300\"", "NaN", "-0", "3.9"];
for (var r = 0; r < RANGES.length; r++) {
  for (var rv = 0; rv < RVALS.length; rv++) {
    var rf = RANGES[r][1];
    var rval = new Function("return " + RVALS[rv])();
    (function (label, fn, a, b) {
      safe("  " + label, function () { return fn(a, b); });
    })(RANGES[r][0] + " on " + RVALS[rv], rf, rval, rval);
  }
}
/* A counter that leaves int32 through a mask-shaped start and repeated
   addition: the interval must be given up rather than followed. */
safe("  counter leaves int32", function () {
  var i = 0x7ffffff0, s = [];
  for (var k = 0; k < 4; k++) { s.push(sh(i) + "/" + sh((i + 16) | 0)); i = i + 16; }
  return s.join(",");
});
safe("  counter masked each step", function () {
  var i = 2147483647, s = [];
  for (var k = 0; k < 3; k++) { s.push(sh(i) + "/" + sh(i & 0xffff)); i = i + i; }
  return s.join(",");
});

print("== case 6: narrowing across edges");
safe("  bound across back edge", function () {
  var i = 0, s = [];
  while (i < 4) { if (i > 1) s.push(sh((i + i) | 0) + "/" + sh(i - 0)); i++; }
  return s.join(",");
});
safe("  bound lost at join", function () {
  var i = 0, out = [];
  for (var k = 0; k < 2; k++) {
    if (k === 0) i = 3; else i = 2147483647;
    if (i < 5) out.push(sh((i + i) | 0)); else out.push(sh(i + i));
  }
  return out.join(",");
});
safe("  bound in try", function () {
  var i = 0, s = [];
  try { while (i < 3) { if (i < 0) throw 1; s.push(sh(i)); i++; } } catch (e) { s.push("caught"); }
  return s.join(",");
});
safe("  bound in catch", function () {
  var i = 6, s = [];
  try { throw 1; } catch (e) { while (i > 0) { s.push(sh(i * 2) + "/" + sh(i - 0)); i--; } }
  return s.join(",");
});
safe("  bound through a label", function () {
  var i = 0, s = [];
  outer: while (i < 5) { i++; if (i === 2) continue outer; s.push(sh(i)); }
  return s.join(",");
});
safe("  bound in a nest", function () {
  var s = [], i, j;
  for (i = 0; i < 3; i++) for (j = i; j < 3; j++) s.push(sh(i * 10 + j) + "/" + sh((j - i) | 0));
  return s.join(",");
});
safe("  bound with a call between", function () {
  var i = 1, s = [];
  function bump(v) { return v + 1; }
  while (i < 4) { s.push(sh(i + 0)); i = bump(i); }
  return s.join(",");
});
safe("  bound with arguments", function () {
  function f(a) { var i = 0, s = []; while (i < a) { s.push(sh(i + 0) + "/" + sh(i + i)); i++; } return s.join(","); }
  return f(3);
});
safe("  narrowed local captured", function () {
  var i = 0, s = [];
  var get = function () { return i; };
  while (i < 3) { s.push(sh(get()) + "/" + sh((i + 0))); i++; }
  return s.join(",");
});
safe("  narrowed arg aliased", function () {
  function f(a) { var s = []; while (a < 3) { s.push(sh(a + 0) + "/" + sh(arguments[0])); a++; } return s.join(","); }
  return f(0);
});
