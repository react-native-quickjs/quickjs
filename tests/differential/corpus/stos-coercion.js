// Differential corpus for the static top-of-stack cache and value coercion.
//
// WHY THIS FILE.  The cached arithmetic fast paths only run when both operands
// are int32 or float64.  Everything else -- NaN, minus zero, an integer that
// spills out of int32, a Symbol, a string, an object whose valueOf runs -- takes
// a slow path, and the slow path has to agree with the cached state about who
// owns the operands.  The printed values are the edge cases.

function show(k, v) { print(k + '=' + v); }

function nan1(a) { return a + NaN; }
show('nan1', nan1(1));
function nan2(a) { return a - undefined; }
show('nan2', nan2(1));
function nan3(a) { return a * 'x'; }
show('nan3', nan3(2));

function negZero1() { var z = 0; return 1 / (z * -3); }
show('negZero1', negZero1());
function negZero2() { return 1 / (0 - 0); }
show('negZero2', negZero2());
function negZero3(a) { return 1 / (a * -1); }
show('negZero3', negZero3(0));
function negZero4() { return 1 / -0; }
show('negZero4', negZero4());

function overflow1(a, b) { return a * b; }
show('overflow1', overflow1(0x7fffffff, 2));
function overflow2(a, b) { return a + b; }
show('overflow2', overflow2(0x7fffffff, 1));
function overflow3(a, b) { return a - b; }
show('overflow3', overflow3(-0x80000000, 1));
function overflow4(a) { return a * a + a; }
show('overflow4', overflow4(0x100000));

function str1(a) { return a + 'x'; }
show('str1', str1(1));
function str2(a) { return 'x' + a + 'y'; }
show('str2', str2(2));
function str3(a) { return (a + 1) + ''; }
show('str3', str3(8));
function str4(a, b) { return a + b; }
show('str4', str4('a', 'b'));

function boolArith(a) { return (a + true) * false; }
show('boolArith', boolArith(3));
function nullArith(a) { return a + null - null; }
show('nullArith', nullArith(4));

function objValueOf(a) {
  var calls = 0;
  var o = { valueOf: function () { calls = calls + 1; return 10; } };
  var r = (a + o) * 2;
  return r + ':' + calls;
}
show('objValueOf', objValueOf(5));

function symPlus(a) {
  var s = Symbol('s');
  try { return a + s; } catch (e) { return 'threw ' + e.name; }
}
show('symPlus', symPlus(1));

function symEq() {
  var s = Symbol('s');
  try { return (s + '') === 'Symbol(s)'; } catch (e) { return 'threw ' + e.name; }
}
show('symEq', symEq());

function symStringConv() {
  var s = Symbol('s');
  return String(s) + '|' + typeof s;
}
show('symStringConv', symStringConv());

function bigintThenNum(a, b) {
  var r = a * b;
  return r + 1n;
}
show('bigintThenNum', bigintThenNum(5n, 5n));

function mixedBigint(a, b) {
  try { return a + b; } catch (e) { return 'threw ' + e.name; }
}
show('mixedBigint', mixedBigint(1n, 2));

function stringCompare(a, b) { return a < b; }
show('stringCompare', stringCompare('a', 'b'));

function coercionChain(a) {
  var t = a + 1;
  t = t + ' ' + (a + 2);
  t = t + ':' + (a * 3);
  return t;
}
show('coercionChain', coercionChain(4));
