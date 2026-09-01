/*
 * Differential corpus for patch 0053 -- the parseInt(<number>) fast path.
 *
 * The fast path answers parseInt() from the numeric argument directly when the
 * radix means decimal, instead of converting the number to a string and parsing
 * it back. It is admitted for JS_TAG_INT arguments and for float64 arguments
 * with 1 <= |d| < 1e21.
 *
 * The interesting cases are the ones just OUTSIDE that window, because for them
 * the string round trip is not equivalent to truncation and the fast path must
 * decline:
 *
 *   parseInt(1e21)  is 1, not 1e21   -- ToString gives "1e+21"
 *   parseInt(1e-7)  is 1, not 0      -- ToString gives "1e-7"
 *   parseInt(-0.5)  is -0, not 0     -- the sign survives a zero result
 *   parseInt(-0)    is +0, not -0    -- ToString(-0) is "0", no sign at all
 *   parseInt(1/0)   is NaN, not Inf  -- "Infinity" is not accepted
 *
 * -0 versus +0 is invisible to === and to String(), so every zero result is
 * printed through a sign probe.  Likewise 1e21 versus 1 would print the same
 * under a lossy formatter, so results are printed with String() which is exact
 * for both.
 *
 * MUTATION-TESTED, 2026-07-30: eight single-guard and paired-guard sabotage
 * builds; see docs/parseint-and-array-species.md, "Mutation testing".  The two
 * that a naive corpus misses are the -0 cases and the 1e21/1e-7 boundary --
 * both are here.
 */

function sign(x) {
  /* distinguishes -0 from +0 without Object.is, which is ES6 */
  if (x === 0) return 1 / x < 0 ? '-0' : '+0';
  return String(x);
}

function show(label, v) { print(label + ' = ' + sign(v)); }

/* ---- integers, no radix ---- */
show('pi(0)', parseInt(0));
show('pi(7)', parseInt(7));
show('pi(-1)', parseInt(-1));
show('pi(2147483647)', parseInt(2147483647));
show('pi(-2147483648)', parseInt(-2147483648));

/* ---- integers with an explicit radix ---- */
show('pi(11,10)', parseInt(11, 10));
show('pi(11,0)', parseInt(11, 0));
show('pi(11,16)', parseInt(11, 16));
show('pi(11,8)', parseInt(11, 8));
show('pi(11,2)', parseInt(11, 2));
show('pi(11,36)', parseInt(11, 36));
show('pi(11,1)', parseInt(11, 1));
show('pi(11,37)', parseInt(11, 37));
show('pi(-11,16)', parseInt(-11, 16));
show('pi(10,undefined)', parseInt(10, undefined));
show('pi(10,null)', parseInt(10, null));
show('pi(10,"10")', parseInt(10, '10'));
show('pi(10,10.9)', parseInt(10, 10.9));
show('pi(10,NaN)', parseInt(10, NaN));
show('pi(10,-0)', parseInt(10, -0));
show('pi(10,1e10)', parseInt(10, 1e10));  /* ToInt32 wraps to 1410065408 */

/* ---- floats INSIDE the fast-path window ---- */
show('pi(1)', parseInt(1.0));
show('pi(1.9)', parseInt(1.9));
show('pi(-1.9)', parseInt(-1.9));
show('pi(3.5)', parseInt(3.5));
show('pi(-3.5)', parseInt(-3.5));
show('pi(1e15)', parseInt(1e15));
show('pi(1e15+0.5)', parseInt(1e15 + 0.5));
show('pi(4503599627370495.5)', parseInt(4503599627370495.5));
show('pi(4503599627370496)', parseInt(4503599627370496));   /* 2^52 */
show('pi(9007199254740993)', parseInt(9007199254740993));   /* 2^53+1 -> 2^53 */
show('pi(1e20)', parseInt(1e20));
show('pi(-1e20)', parseInt(-1e20));
show('pi(123456789012345678901)', parseInt(123456789012345678901));
show('pi(1.7e20)', parseInt(1.7e20));
show('pi(-1.7e20)', parseInt(-1.7e20));
show('pi(1.9,10)', parseInt(1.9, 10));
show('pi(1.9,0)', parseInt(1.9, 0));
show('pi(1.9,16)', parseInt(1.9, 16));
show('pi(255.9,16)', parseInt(255.9, 16));

/* ---- floats OUTSIDE the window: the boundary that kills a wrong guard ---- */
show('pi(1e21)', parseInt(1e21));                 /* -> 1 */
show('pi(-1e21)', parseInt(-1e21));               /* -> -1 */
show('pi(1e22)', parseInt(1e22));                 /* -> 1 */
show('pi(9.99e20)', parseInt(9.99e20));           /* still fixed: 999e18 */
show('pi(1e-7)', parseInt(1e-7));                 /* -> 1 */
show('pi(-1e-7)', parseInt(-1e-7));               /* -> -1 */
show('pi(1e-6)', parseInt(1e-6));                 /* "0.000001" -> 0 */
show('pi(0.5)', parseInt(0.5));                   /* -> +0 */
show('pi(-0.5)', parseInt(-0.5));                 /* -> -0  <-- */
show('pi(-0.0)', parseInt(-0.0));                 /* -> +0  <-- */
show('pi(0.0)', parseInt(0.0));
show('pi(-0.999)', parseInt(-0.999));             /* -> -0 */
show('pi(NaN)', parseInt(NaN));
show('pi(Infinity)', parseInt(1 / 0));
show('pi(-Infinity)', parseInt(-1 / 0));
show('pi(5e-324)', parseInt(5e-324));             /* min subnormal -> 5 */
show('pi(1.5e-10)', parseInt(1.5e-10));           /* "1.5e-10" -> 1 */
show('pi(-0.5,16)', parseInt(-0.5, 16));
show('pi(1e21,16)', parseInt(1e21, 16));          /* "1e+21" radix16 -> 30 */

/* ---- strings must be untouched by the fast path ---- */
show('pi("42")', parseInt('42'));
show('pi("  42  ")', parseInt('  42  '));
show('pi("\\t\\n\\r\\f\\v 42")', parseInt('\t\n\r\f\v 42'));
show('pi("+42")', parseInt('+42'));
show('pi("-42")', parseInt('-42'));
show('pi("42px")', parseInt('42px'));
show('pi("0x1f")', parseInt('0x1f'));
show('pi("0X1f")', parseInt('0X1f'));
show('pi("0x1f",16)', parseInt('0x1f', 16));
show('pi("0x1f",10)', parseInt('0x1f', 10));
show('pi("-0x1f")', parseInt('-0x1f'));
show('pi("+0x1f")', parseInt('+0x1f'));
show('pi("08")', parseInt('08'));
show('pi("010",8)', parseInt('010', 8));
show('pi("")', parseInt(''));
show('pi(" ")', parseInt(' '));
show('pi("-")', parseInt('-'));
show('pi("Infinity")', parseInt('Infinity'));
show('pi("-0")', parseInt('-0'));
show('pi("-0.5")', parseInt('-0.5'));
show('pi("1e21")', parseInt('1e21'));
show('pi("zz",36)', parseInt('zz', 36));
show('pi("ZZ",36)', parseInt('ZZ', 36));
show('pi("中3")', parseInt('中3'));
show('pi("1中")', parseInt('1中'));

/* ---- non-number, non-string arguments ---- */
show('pi(undefined)', parseInt(undefined));
show('pi(null)', parseInt(null));
show('pi(true)', parseInt(true));
show('pi(false)', parseInt(false));
show('pi([])', parseInt([]));
show('pi([7])', parseInt([7]));
show('pi([7,8])', parseInt([7, 8]));
show('pi({})', parseInt({}));

/* ---- side-effect ORDER: ToString(value) must precede ToInt32(radix) ---- */
var log = [];
var v = { toString: function () { log.push('value'); return '20'; } };
var r = { valueOf: function () { log.push('radix'); return 16; } };
show('pi(objs)', parseInt(v, r));
print('order = ' + log.join(','));

/* an object radix whose valueOf throws must throw AFTER ToString ran */
log = [];
var v2 = { toString: function () { log.push('value'); return '20'; } };
var r2 = { valueOf: function () { throw new Error('boom'); } };
try { parseInt(v2, r2); print('no throw'); }
catch (e) { print('threw ' + e.message + ' after ' + log.join(',')); }

/* a value whose toString throws must throw before the radix is touched */
log = [];
var v3 = { toString: function () { throw new Error('vboom'); } };
var r3 = { valueOf: function () { log.push('radix'); return 10; } };
try { parseInt(v3, r3); print('no throw'); }
catch (e) { print('threw ' + e.message + ' touched [' + log.join(',') + ']'); }

/* ---- Number.parseInt must be the same function ---- */
print('same = ' + (Number.parseInt === parseInt));
show('Number.parseInt(7)', Number.parseInt(7));
show('Number.parseInt(1.9)', Number.parseInt(1.9));

/* ---- a value round trip: fast path result must be a Number, not a String ---- */
print('typeof = ' + typeof parseInt(7) + ',' + typeof parseInt(1.9));
print('tag = ' + (parseInt(1e20) === 1e20) + ',' + (parseInt(2.5) === 2));
